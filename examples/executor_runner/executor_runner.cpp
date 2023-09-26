/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * @file
 *
 * This tool can run ExecuTorch model files that only use operators that
 * are covered by the portable kernels, with possible delegate to the
 * test_backend_compiler_lib.
 *
 * It sets all input tensor data to ones, and assumes that the outputs are
 * all fp32 tensors.
 */

#include <iostream>
#include <memory>
#include <string>

#include <gflags/gflags.h>

#include <executorch/extension/data_loader/file_data_loader.h>
#include <executorch/extension/evalue_util/print_evalue.h>
#include <executorch/runtime/executor/method.h>
#include <executorch/runtime/executor/program.h>
#include <executorch/runtime/platform/log.h>
#include <executorch/runtime/platform/profiler.h>
#include <executorch/runtime/platform/runtime.h>
#include <executorch/util/util.h>

#include <fbjni/fbjni.h>

static uint8_t method_allocator_pool[4 * 1024U * 1024U]; // 4 MB

DEFINE_string(
    model_path,
    "model.pte",
    "Model serialized in flatbuffer format.");
DEFINE_string(
    prof_result_path,
    "prof_result.bin",
    "ExecuTorch profiler output path.");

using namespace torch::executor;
using torch::executor::util::FileDataLoader;

int main(int argc, char** argv) {
  runtime_init();

  gflags::ParseCommandLineFlags(&argc, &argv, true);
  if (argc != 1) {
    std::string msg = "Extra commandline args:";
    for (int i = 1 /* skip argv[0] (program name) */; i < argc; i++) {
      msg += std::string(" ") + argv[i];
    }
    ET_LOG(Error, "%s", msg.c_str());
    return 1;
  }

  // Create a loader to get the data of the program file. There are other
  // DataLoaders that use mmap() or point to data that's already in memory, and
  // users can create their own DataLoaders to load from arbitrary sources.
  const char* model_path = FLAGS_model_path.c_str();
  Result<FileDataLoader> loader = FileDataLoader::from(model_path);
  ET_CHECK_MSG(
      loader.ok(), "FileDataLoader::from() failed: 0x%" PRIx32, loader.error());

  // Parse the program file. This is immutable, and can also be reused between
  // multiple execution invocations across multiple threads.
  Result<Program> program = Program::load(&loader.get());
  if (!program.ok()) {
    ET_LOG(Error, "Failed to parse model file %s", model_path);
  }
  ET_LOG(Info, "Model file %s is loaded.", model_path);

  // Use the first method in the program.
  const char* method_name = nullptr;
  {
    const auto method_name_result = program->get_method_name(0);
    ET_CHECK_MSG(method_name_result.ok(), "Program has no methods");
    method_name = *method_name_result;
  }
  ET_LOG(Info, "Using method %s", method_name);

  // MethodMeta describes the memory requirements of the method.
  Result<MethodMeta> method_meta = program->method_meta(method_name);
  ET_CHECK_MSG(
      method_meta.ok(),
      "Failed to get method_meta for %s: 0x%x",
      method_name,
      (unsigned int)method_meta.error());

  //
  // The runtime does not use malloc/new; it allocates all memory using the
  // MemoryManger provided by the client. Clients are responsible for allocating
  // the memory ahead of time, or providing MemoryAllocator subclasses that can
  // do it dynamically.
  //

  // The method allocator is used to allocate all dynamic C++ metadata/objects
  // used to represent the loaded method. This allocator is only used during
  // loading a method of the program, which will return an error if there was
  // not enough memory.
  //
  // The amount of memory required depends on the loaded method and the runtime
  // code itself. The amount of memory here is usually determined by running the
  // method and seeing how much memory is actually used, though it's possible to
  // subclass MemoryAllocator so that it calls malloc() under the hood (see
  // MallocMemoryAllocator).
  //
  // In this example we use a statically allocated memory pool.
  MemoryAllocator method_allocator{
      MemoryAllocator(sizeof(method_allocator_pool), method_allocator_pool)};
  method_allocator.enable_profiling("method allocator");

  // The memory-planned buffers will back the mutable tensors used by the
  // method. The sizes of these buffers were determined ahead of time during the
  // memory-planning pasees.
  //
  // Each buffer typically corresponds to a different hardware memory bank. Most
  // mobile environments will only have a single buffer. Some embedded
  // environments may have more than one for, e.g., slow/large DRAM and
  // fast/small SRAM, or for memory associated with particular cores.
  std::vector<std::unique_ptr<uint8_t[]>> planned_buffers; // Owns the memory
  std::vector<Span<uint8_t>> planned_spans; // Passed to the allocator
  size_t num_memory_planned_buffers = method_meta->num_memory_planned_buffers();
  for (size_t id = 0; id < num_memory_planned_buffers; ++id) {
    // .get() will always succeed because id < num_memory_planned_buffers.
    size_t buffer_size =
        static_cast<size_t>(method_meta->memory_planned_buffer_size(id).get());
    ET_LOG(Info, "Setting up planned buffer %zu, size %zu.", id, buffer_size);
    planned_buffers.push_back(std::make_unique<uint8_t[]>(buffer_size));
    planned_spans.push_back({planned_buffers.back().get(), buffer_size});
  }
  HierarchicalAllocator planned_memory(
      {planned_spans.data(), planned_spans.size()});

  // Assemble all of the allocators into the MemoryManager that the Executor
  // will use.
  MemoryManager memory_manager(&method_allocator, &planned_memory);

  //
  // Load the method from the program, using the provided allocators. Running
  // the method can mutate the memory-planned buffers, so the method should only
  // be used by a single thread at at time, but it can be reused.
  //

  Result<Method> method = program->load_method(method_name, &memory_manager);
  ET_CHECK_MSG(
      method.ok(),
      "Loading of method %s failed with status 0x%" PRIx32,
      method_name,
      method.error());
  ET_LOG(Info, "Method loaded.");

  // Prepare the inputs.
  // Use ones-initialized inputs.
  auto inputs = util::PrepareInputTensors(*method);
  ET_LOG(Info, "Inputs prepared.");

  // Run the model.
  Error status = method->execute();
  ET_CHECK_MSG(
      status == Error::Ok,
      "Execution of method %s failed with status 0x%" PRIx32,
      method_name,
      status);
  ET_LOG(Info, "Model executed successfully.");

  // Print the outputs.
  std::vector<EValue> outputs(method->outputs_size());
  ET_LOG(Info, "%zu outputs: ", outputs.size());
  status = method->get_outputs(outputs.data(), outputs.size());
  ET_CHECK(status == Error::Ok);
  // Print the first and last 100 elements of long lists of scalars.
  std::cout << torch::executor::util::evalue_edge_items(100);
  for (int i = 0; i < outputs.size(); ++i) {
    std::cout << "Output " << i << ": " << outputs[i] << std::endl;
  }

  // Dump the profiling data to the specified file.
  torch::executor::prof_result_t prof_result;
  EXECUTORCH_DUMP_PROFILE_RESULTS(&prof_result);
  if (prof_result.num_bytes != 0) {
    FILE* ptr = fopen(FLAGS_prof_result_path.c_str(), "w+");
    fwrite(prof_result.prof_data, 1, prof_result.num_bytes, ptr);
    fclose(ptr);
  }

  util::FreeInputs(inputs);
  return 0;
}

#include <cassert>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>

#ifdef __ANDROID__
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#endif


namespace pytorch_jni {

    class JEValue : public facebook::jni::JavaClass<JEValue> {

    public:
        constexpr static const char* kJavaDescriptor = "Lcom/example/executorchdemo/executor/EValue;";

        constexpr static int kTypeCodeNull = 1;

        constexpr static int kTypeCodeTensor = 2;
        constexpr static int kTypeCodeBool = 3;
        constexpr static int kTypeCodeLong = 4;
        constexpr static int kTypeCodeDouble = 5;
        constexpr static int kTypeCodeString = 6;

        constexpr static int kTypeCodeTuple = 7;
        constexpr static int kTypeCodeBoolList = 8;
        constexpr static int kTypeCodeLongList = 9;
        constexpr static int kTypeCodeDoubleList = 10;
        constexpr static int kTypeCodeTensorList = 11;
        constexpr static int kTypeCodeList = 12;

        constexpr static int kTypeCodeDictStringKey = 13;
        constexpr static int kTypeCodeDictLongKey = 14;

    };

    class PytorchJni : public facebook::jni::HybridClass<PytorchJni> {
    private:
        std::string model_path_;
        friend HybridBase;

    public:
        constexpr static auto kJavaDescriptor = "Lcom/example/executorchdemo/executor/NativePeer;";

        static facebook::jni::local_ref<jhybriddata> initHybrid(
                facebook::jni::alias_ref<jclass>,
                facebook::jni::alias_ref<jstring> modelPath,
                facebook::jni::alias_ref<
                facebook::jni::JMap<facebook::jni::JString, facebook::jni::JString>>
                extraFiles) {
            return makeCxxInstance(modelPath, extraFiles);
        }

#ifdef __ANDROID__
        static facebook::jni::local_ref<jhybriddata> initHybridAndroidAsset(
      facebook::jni::alias_ref<jclass>,
      facebook::jni::alias_ref<jstring> assetName,
      facebook::jni::alias_ref<jobject> assetManager) {
    return makeCxxInstance(assetName, assetManager);
  }
#endif

        PytorchJni(
                facebook::jni::alias_ref<jstring> modelPath,
                facebook::jni::alias_ref<
                facebook::jni::JMap<facebook::jni::JString, facebook::jni::JString>>
                extraFiles) {
            std::unordered_map<std::string, std::string> extra_files;
            const auto has_extra = extraFiles && extraFiles->size() > 0;
            if (has_extra) {
                for (const auto& e : *extraFiles) {
                    extra_files[e.first->toStdString()] = "";
                }
            }

            model_path_ = modelPath->toStdString();
        }

#ifdef __ANDROID__
        PytorchJni(
      facebook::jni::alias_ref<jstring> assetName,
      facebook::jni::alias_ref<jobject> assetManager) {
    JNIEnv* env = facebook::jni::Environment::current();
    AAssetManager* mgr = AAssetManager_fromJava(env, assetManager.get());
    if (!mgr) {
      facebook::jni::throwNewJavaException(
          facebook::jni::gJavaLangIllegalArgumentException,
          "Unable to get asset manager");
    }
    AAsset* asset = AAssetManager_open(
        mgr, assetName->toStdString().c_str(), AASSET_MODE_BUFFER);
    if (!asset) {
      facebook::jni::throwNewJavaException(
          facebook::jni::gJavaLangIllegalArgumentException,
          "Failed to open asset '%s'",
          assetName->toStdString().c_str());
    }
    auto assetBuffer = AAsset_getBuffer(asset);
    if (!assetBuffer) {
      facebook::jni::throwNewJavaException(
          facebook::jni::gJavaLangIllegalArgumentException,
          "Could not get buffer for asset '%s'",
          assetName->toStdString().c_str());
    }
  }
#endif

        static void registerNatives() {
            registerHybrid({
                                   makeNativeMethod("initHybrid", PytorchJni::initHybrid),
#ifdef __ANDROID__
                                   makeNativeMethod(
            "initHybridAndroidAsset", PytorchJni::initHybridAndroidAsset),
#endif
                                   makeNativeMethod("forward", PytorchJni::forward),
                           });
        }

        facebook::jni::local_ref<JEValue> forward(
                facebook::jni::alias_ref<
                facebook::jni::JArrayClass<JEValue::javaobject>::javaobject>
                jinputs) {

            const char* model_path = model_path_.c_str();
            Result<FileDataLoader> loader = FileDataLoader::from(model_path);
              ET_CHECK_MSG(
      loader.ok(), "FileDataLoader::from() failed: 0x%" PRIx32, loader.error());

              Result<Program> program = Program::load(&loader.get());
              if (!program.ok()) {
                ET_LOG(Error, "Failed to parse model file %s", model_path);
              }
              ET_LOG(Info, "Model file %s is loaded.", model_path);

  // Use the first method in the program.
  const char* method_name = nullptr;
  {
    const auto method_name_result = program->get_method_name(0);
    ET_CHECK_MSG(method_name_result.ok(), "Program has no methods");
    method_name = *method_name_result;
  }
  ET_LOG(Info, "Using method %s", method_name);

  // MethodMeta describes the memory requirements of the method.
  Result<MethodMeta> method_meta = program->method_meta(method_name);
  ET_CHECK_MSG(
      method_meta.ok(),
      "Failed to get method_meta for %s: 0x%x",
      method_name,
      (unsigned int)method_meta.error());

  //
  // The runtime does not use malloc/new; it allocates all memory using the
  // MemoryManger provided by the client. Clients are responsible for allocating
  // the memory ahead of time, or providing MemoryAllocator subclasses that can
  // do it dynamically.
  //

  // The method allocator is used to allocate all dynamic C++ metadata/objects
  // used to represent the loaded method. This allocator is only used during
  // loading a method of the program, which will return an error if there was
  // not enough memory.
  //
  // The amount of memory required depends on the loaded method and the runtime
  // code itself. The amount of memory here is usually determined by running the
  // method and seeing how much memory is actually used, though it's possible to
  // subclass MemoryAllocator so that it calls malloc() under the hood (see
  // MallocMemoryAllocator).
  //
  // In this example we use a statically allocated memory pool.
  MemoryAllocator method_allocator{
      MemoryAllocator(sizeof(method_allocator_pool), method_allocator_pool)};
  method_allocator.enable_profiling("method allocator");

  // The memory-planned buffers will back the mutable tensors used by the
  // method. The sizes of these buffers were determined ahead of time during the
  // memory-planning pasees.
  //
  // Each buffer typically corresponds to a different hardware memory bank. Most
  // mobile environments will only have a single buffer. Some embedded
  // environments may have more than one for, e.g., slow/large DRAM and
  // fast/small SRAM, or for memory associated with particular cores.
  std::vector<std::unique_ptr<uint8_t[]>> planned_buffers; // Owns the memory
  std::vector<Span<uint8_t>> planned_spans; // Passed to the allocator
  size_t num_memory_planned_buffers = method_meta->num_memory_planned_buffers();
  for (size_t id = 0; id < num_memory_planned_buffers; ++id) {
    // .get() will always succeed because id < num_memory_planned_buffers.
    size_t buffer_size =
        static_cast<size_t>(method_meta->memory_planned_buffer_size(id).get());
    ET_LOG(Info, "Setting up planned buffer %zu, size %zu.", id, buffer_size);
    planned_buffers.push_back(std::make_unique<uint8_t[]>(buffer_size));
    planned_spans.push_back({planned_buffers.back().get(), buffer_size});
  }
  HierarchicalAllocator planned_memory(
      {planned_spans.data(), planned_spans.size()});

  // Assemble all of the allocators into the MemoryManager that the Executor
  // will use.
  MemoryManager memory_manager(&method_allocator, &planned_memory);

  //
  // Load the method from the program, using the provided allocators. Running
  // the method can mutate the memory-planned buffers, so the method should only
  // be used by a single thread at at time, but it can be reused.
  //

  Result<Method> method = program->load_method(method_name, &memory_manager);
  ET_CHECK_MSG(
      method.ok(),
      "Loading of method %s failed with status 0x%" PRIx32,
      method_name,
      method.error());
  ET_LOG(Info, "Method loaded.");

  // Prepare the inputs.
  // Use ones-initialized inputs.
  // ****************** TODO *************************************************
  auto inputs = util::PrepareInputTensors(*method);
  ET_LOG(Info, "Inputs prepared.");

  // Run the model.
  Error status = method->execute();
  ET_CHECK_MSG(
      status == Error::Ok,
      "Execution of method %s failed with status 0x%" PRIx32,
      method_name,
      status);
  ET_LOG(Info, "Model executed successfully.");

  // Print the outputs.
  std::vector<EValue> outputs(method->outputs_size());
  ET_LOG(Info, "%zu outputs: ", outputs.size());
  status = method->get_outputs(outputs.data(), outputs.size());
  ET_CHECK(status == Error::Ok);
  // Print the first and last 100 elements of long lists of scalars.
  std::cout << torch::executor::util::evalue_edge_items(100);
  for (int i = 0; i < outputs.size(); ++i) {
    std::cout << "Output " << i << ": " << outputs[i] << std::endl;
  }

  util::FreeInputs(inputs);

  static auto jMethodOptionalNull =
        JEValue::javaClassStatic()
            ->getStaticMethod<facebook::jni::local_ref<JEValue>()>(
                "optionalNull");
  return jMethodOptionalNull(JEValue::javaClassStatic());

          //  std::vector<EValue> inputs{};
          //  size_t n = jinputs->size();
          //  inputs.reserve(n);
          //  for (size_t i = 0; i < n; i++) {
          //      at::EValue atEValue = JEValue::JEValueToAtEValue(jinputs->getElement(i));
          //      inputs.push_back(std::move(atEValue));
          //  }

          //  auto output = [&]() {
          //      LiteJITCallGuard guard;
          //      return module_.forward(inputs);
          //  }();
          //  return JEValue::newJEValueFromAtEValue(output);
        }

    };

    void common_registerNatives() {
        static const int once = []() {
#if defined(__ANDROID__)
            return 0;
//            pytorch_jni::PyTorchAndroidJni::registerNatives();
#endif
            return 0;
        }();
        ((void)once);
    }


} // namespace pytorch_jni

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    return facebook::jni::initialize(vm, [] {
        pytorch_jni::common_registerNatives();
        pytorch_jni::PytorchJni::registerNatives();
    });
}

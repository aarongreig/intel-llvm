//==------------------- backend.cpp ----------------------------------------==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "detail/context_impl.hpp"
#include "detail/event_impl.hpp"
#include "detail/kernel_bundle_impl.hpp"
#include "detail/kernel_id_impl.hpp"
#include "detail/platform_impl.hpp"
#include "detail/plugin.hpp"
#include "detail/queue_impl.hpp"
#include "sycl/detail/impl_utils.hpp"
#include <sycl/backend.hpp>
#include <sycl/detail/common.hpp>
#include <sycl/detail/export.hpp>
#include <sycl/detail/pi.h>
#include <sycl/detail/pi.hpp>
#include <sycl/exception.hpp>
#include <sycl/exception_list.hpp>
#include <sycl/kernel_bundle.hpp>

#include <algorithm>
#include <memory>

namespace sycl {
inline namespace _V1 {
namespace detail {

static const UrPluginPtr &getUrPlugin(backend Backend) {
  switch (Backend) {
  case backend::opencl:
    return pi::getUrPlugin<backend::opencl>();
  case backend::ext_oneapi_level_zero:
    return pi::getUrPlugin<backend::ext_oneapi_level_zero>();
  case backend::ext_oneapi_cuda:
    return pi::getUrPlugin<backend::ext_oneapi_cuda>();
  case backend::ext_oneapi_hip:
    return pi::getUrPlugin<backend::ext_oneapi_hip>();
  default:
    throw sycl::exception(sycl::make_error_code(sycl::errc::runtime),
                          "getUrPlugin: Unsupported backend " +
                              detail::codeToString(PI_ERROR_INVALID_OPERATION));
  }
}

backend convertBackend(pi_platform_backend PiBackend) {
  switch (PiBackend) {
  case PI_EXT_PLATFORM_BACKEND_UNKNOWN:
    return backend::all; // No specific backend
  case PI_EXT_PLATFORM_BACKEND_LEVEL_ZERO:
    return backend::ext_oneapi_level_zero;
  case PI_EXT_PLATFORM_BACKEND_OPENCL:
    return backend::opencl;
  case PI_EXT_PLATFORM_BACKEND_CUDA:
    return backend::ext_oneapi_cuda;
  case PI_EXT_PLATFORM_BACKEND_HIP:
    return backend::ext_oneapi_hip;
  case PI_EXT_PLATFORM_BACKEND_ESIMD:
    return backend::ext_intel_esimd_emulator;
  case PI_EXT_PLATFORM_BACKEND_NATIVE_CPU:
    return backend::ext_oneapi_native_cpu;
  }
  throw sycl::runtime_error{"convertBackend: Unsupported backend",
                            PI_ERROR_INVALID_OPERATION};
}

backend convertUrBackend(ur_platform_backend_t UrBackend) {
  switch (UrBackend) {
  case UR_PLATFORM_BACKEND_LEVEL_ZERO:
    return backend::ext_oneapi_level_zero;
  case UR_PLATFORM_BACKEND_OPENCL:
    return backend::opencl;
  case UR_PLATFORM_BACKEND_CUDA:
    return backend::ext_oneapi_cuda;
  case UR_PLATFORM_BACKEND_HIP:
    return backend::ext_oneapi_hip;
  case UR_PLATFORM_BACKEND_NATIVE_CPU:
    return backend::ext_oneapi_native_cpu;
  default:
    // no idea what to do here
    return backend::all;
  }
}

platform make_platform(ur_native_handle_t NativeHandle, backend Backend) {
  const auto &Plugin = getUrPlugin(Backend);

  // Create UR platform first.
  ur_platform_handle_t UrPlatform = nullptr;
  Plugin->call(urPlatformCreateWithNativeHandle, NativeHandle, nullptr,
               &UrPlatform);

  return detail::createSyclObjFromImpl<platform>(
      platform_impl::getOrMakePlatformImpl(UrPlatform, Plugin));
}

__SYCL_EXPORT device make_device(ur_native_handle_t NativeHandle,
                                 backend Backend) {
  const auto &Plugin = getUrPlugin(Backend);

  ur_device_handle_t UrDevice = nullptr;
  Plugin->call(urDeviceCreateWithNativeHandle, NativeHandle, nullptr, nullptr,
               &UrDevice);
  // Construct the SYCL device from PI device.
  return detail::createSyclObjFromImpl<device>(
      std::make_shared<device_impl>(UrDevice, Plugin));
}

__SYCL_EXPORT context make_context(ur_native_handle_t NativeHandle,
                                   const async_handler &Handler,
                                   backend Backend) {
  const auto &Plugin = getUrPlugin(Backend);

  ur_context_handle_t UrContext = nullptr;
  ur_context_native_properties_t Properties{};
  Properties.stype = UR_STRUCTURE_TYPE_CONTEXT_NATIVE_PROPERTIES;
  Properties.isNativeHandleOwned = false;
  Plugin->call(urContextCreateWithNativeHandle, NativeHandle, 0, nullptr,
               &Properties, &UrContext);
  // Construct the SYCL context from PI context.
  return detail::createSyclObjFromImpl<context>(
      std::make_shared<context_impl>(UrContext, Handler, Plugin));
}

__SYCL_EXPORT queue make_queue(ur_native_handle_t NativeHandle,
                               int32_t NativeHandleDesc, const context &Context,
                               const device *Device, bool KeepOwnership,
                               const property_list &PropList,
                               const async_handler &Handler, backend Backend) {
  ur_device_handle_t UrDevice =
      Device ? getSyclObjImpl(*Device)->getUrHandleRef() : nullptr;
  const auto &Plugin = getUrPlugin(Backend);
  const auto &ContextImpl = getSyclObjImpl(Context);

  if (PropList.has_property<ext::intel::property::queue::compute_index>()) {
    throw sycl::exception(
        make_error_code(errc::invalid),
        "Queue create using make_queue cannot have compute_index property.");
  }

  ur_queue_native_desc_t Desc{};
  Desc.stype = UR_STRUCTURE_TYPE_QUEUE_NATIVE_DESC;
  Desc.pNativeData = &NativeHandleDesc;

  ur_queue_properties_t Properties{};
  Properties.stype = UR_STRUCTURE_TYPE_QUEUE_PROPERTIES;
  Properties.flags = queue_impl::createUrQueueFlags(
      PropList, PropList.has_property<property::queue::in_order>()
                    ? QueueOrder::Ordered
                    : QueueOrder::OOO);

  ur_queue_native_properties_t NativeProperties{};
  NativeProperties.stype = UR_STRUCTURE_TYPE_QUEUE_NATIVE_PROPERTIES;
  NativeProperties.isNativeHandleOwned = !KeepOwnership;

  Properties.pNext = &Desc;
  NativeProperties.pNext = &Properties;

  // Create UR queue first.
  ur_queue_handle_t UrQueue = nullptr;

  Plugin->call(urQueueCreateWithNativeHandle, NativeHandle,
               ContextImpl->getUrHandleRef(), UrDevice, &NativeProperties,
               &UrQueue);
  // Construct the SYCL queue from UR queue.
  return detail::createSyclObjFromImpl<queue>(
      std::make_shared<queue_impl>(UrQueue, ContextImpl, Handler, PropList));
}

__SYCL_EXPORT event make_event(ur_native_handle_t NativeHandle,
                               const context &Context, backend Backend) {
  return make_event(NativeHandle, Context, false, Backend);
}

__SYCL_EXPORT event make_event(ur_native_handle_t NativeHandle,
                               const context &Context, bool KeepOwnership,
                               backend Backend) {
  const auto &Plugin = getUrPlugin(Backend);
  const auto &ContextImpl = getSyclObjImpl(Context);

    ur_event_handle_t UrEvent = nullptr;
    ur_event_native_properties_t Properties{};
    Properties.stype = UR_STRUCTURE_TYPE_EVENT_NATIVE_PROPERTIES;
    Properties.isNativeHandleOwned = !KeepOwnership;
  
    Plugin->call(urEventCreateWithNativeHandle,
        NativeHandle, ContextImpl->getUrHandleRef(), &Properties, &UrEvent);
    event Event = detail::createSyclObjFromImpl<event>(
        std::make_shared<event_impl>(UrEvent, Context));

    if (Backend == backend::opencl)
      Plugin->call(urEventRetain, UrEvent);
  return Event;
}

std::shared_ptr<detail::kernel_bundle_impl>
make_kernel_bundle(ur_native_handle_t NativeHandle,
                   const context &TargetContext, bool KeepOwnership,
                   bundle_state State, backend Backend) {
  const auto &Plugin = getUrPlugin(Backend);
  const auto &ContextImpl = getSyclObjImpl(TargetContext);

  ur_program_handle_t UrProgram = nullptr;
  ur_program_native_properties_t Properties{};
  Properties.stype = UR_STRUCTURE_TYPE_PROGRAM_NATIVE_PROPERTIES;
  Properties.isNativeHandleOwned = !KeepOwnership;

  Plugin->call(urProgramCreateWithNativeHandle, NativeHandle,
               ContextImpl->getUrHandleRef(), &Properties, &UrProgram);
  if (ContextImpl->getBackend() == backend::opencl)
    Plugin->call(urProgramRetain, UrProgram);

  std::vector<ur_device_handle_t> ProgramDevices;
  uint32_t NumDevices = 0;

  Plugin->call(urProgramGetInfo, UrProgram, UR_PROGRAM_INFO_NUM_DEVICES,
               sizeof(NumDevices), &NumDevices, nullptr);
  ProgramDevices.resize(NumDevices);
  Plugin->call(urProgramGetInfo, UrProgram, UR_PROGRAM_INFO_DEVICES,
               sizeof(ur_device_handle_t) * NumDevices, ProgramDevices.data(),
               nullptr);

  for (auto &Dev : ProgramDevices) {
    ur_program_binary_type_t BinaryType;
    Plugin->call(urProgramGetBuildInfo, UrProgram, Dev,
                 UR_PROGRAM_BUILD_INFO_BINARY_TYPE,
                 sizeof(ur_program_binary_type_t), &BinaryType, nullptr);
    switch (BinaryType) {
    case (UR_PROGRAM_BINARY_TYPE_NONE):
      if (State == bundle_state::object) {
        auto Res = Plugin->call_nocheck(urProgramCompileExp, UrProgram, 1, &Dev,
                                        nullptr);
        if (Res == UR_RESULT_ERROR_UNSUPPORTED_FEATURE) {
          Res = Plugin->call_nocheck(urProgramCompile,
                                     ContextImpl->getUrHandleRef(), UrProgram,
                                     nullptr);
        }
        Plugin->checkUrResult<errc::build>(Res);
      }

      else if (State == bundle_state::executable) {
        auto Res = Plugin->call_nocheck(urProgramBuildExp, UrProgram, 1, &Dev,
                                        nullptr);
        if (Res == UR_RESULT_ERROR_UNSUPPORTED_FEATURE) {
          Res = Plugin->call_nocheck(urProgramBuild,
                                     ContextImpl->getUrHandleRef(), UrProgram,
                                     nullptr);
        }
        Plugin->checkUrResult<errc::build>(Res);
      }

      break;
    case (UR_PROGRAM_BINARY_TYPE_COMPILED_OBJECT):
    case (UR_PROGRAM_BINARY_TYPE_LIBRARY):
      if (State == bundle_state::input)
        throw sycl::exception(sycl::make_error_code(sycl::errc::runtime),
                              "Program and kernel_bundle state mismatch " +
                                  detail::codeToString(PI_ERROR_INVALID_VALUE));
      if (State == bundle_state::executable) {
        auto Res = Plugin->call_nocheck(urProgramLinkExp,
                                        ContextImpl->getUrHandleRef(), 1, &Dev,
                                        1, &UrProgram, nullptr, &UrProgram);
        if (Res == UR_RESULT_ERROR_UNSUPPORTED_FEATURE) {
          Res =
              Plugin->call_nocheck(urProgramLink, ContextImpl->getUrHandleRef(),
                                   1, &UrProgram, nullptr, &UrProgram);
        }
        Plugin->checkUrResult<errc::build>(Res);
      }
      break;
    case (UR_PROGRAM_BINARY_TYPE_EXECUTABLE):
      if (State == bundle_state::input || State == bundle_state::object)
        throw sycl::exception(sycl::make_error_code(sycl::errc::runtime),
                              "Program and kernel_bundle state mismatch " +
                                  detail::codeToString(PI_ERROR_INVALID_VALUE));
      break;
    default:
      break;
    }
  }

  std::vector<device> Devices;
  Devices.reserve(ProgramDevices.size());
  std::transform(
      ProgramDevices.begin(), ProgramDevices.end(), std::back_inserter(Devices),
      [&Plugin](const auto &Dev) {
        auto Platform =
            detail::platform_impl::getPlatformFromUrDevice(Dev, Plugin);
        auto DeviceImpl = Platform->getOrMakeDeviceImpl(Dev, Platform);
        return createSyclObjFromImpl<device>(DeviceImpl);
      });

  // Unlike SYCL, other backends, like OpenCL or Level Zero, may not support
  // getting kernel IDs before executable is built. The SYCL Runtime workarounds
  // this by pre-building the device image and extracting kernel info. We can't
  // do the same to user images, since they may contain references to undefined
  // symbols (e.g. when kernel_bundle is supposed to be joined with another).
  auto KernelIDs = std::make_shared<std::vector<kernel_id>>();
  auto DevImgImpl = std::make_shared<device_image_impl>(
      nullptr, TargetContext, Devices, State, KernelIDs, UrProgram);
  device_image_plain DevImg{DevImgImpl};

  return std::make_shared<kernel_bundle_impl>(TargetContext, Devices, DevImg);
}

// TODO: Unused. Remove when allowed.
std::shared_ptr<detail::kernel_bundle_impl>
make_kernel_bundle(ur_native_handle_t NativeHandle,
                   const context &TargetContext, bundle_state State,
                   backend Backend) {
  return make_kernel_bundle(NativeHandle, TargetContext, false, State, Backend);
}

kernel make_kernel(const context &TargetContext,
                   const kernel_bundle<bundle_state::executable> &KernelBundle,
                   ur_native_handle_t NativeHandle, bool KeepOwnership,
                   backend Backend) {
  const auto &Plugin = getUrPlugin(Backend);
  const auto &ContextImpl = getSyclObjImpl(TargetContext);
  const auto KernelBundleImpl = getSyclObjImpl(KernelBundle);

  // For Level-Zero expect exactly one device image in the bundle. This is
  // natural for interop kernel to get created out of a single native
  // program/module. This way we don't need to search the exact device image for
  // the kernel, which may not be trivial.
  //
  // Other backends don't need PI program.
  //
  ur_program_handle_t UrProgram = nullptr;
  if (Backend == backend::ext_oneapi_level_zero) {
    if (KernelBundleImpl->size() != 1)
      throw sycl::exception(
          sycl::make_error_code(sycl::errc::runtime),
          "make_kernel: kernel_bundle must have single program image " +
              detail::codeToString(PI_ERROR_INVALID_PROGRAM));

    const device_image<bundle_state::executable> &DeviceImage =
        *KernelBundle.begin();
    const auto &DeviceImageImpl = getSyclObjImpl(DeviceImage);
    UrProgram = DeviceImageImpl->get_ur_program_ref();
  }

  // Create PI kernel first.
  ur_kernel_handle_t UrKernel = nullptr;
  ur_kernel_native_properties_t Properties{};
  Properties.stype = UR_STRUCTURE_TYPE_KERNEL_NATIVE_PROPERTIES;
  Properties.isNativeHandleOwned = !KeepOwnership;
  Plugin->call(urKernelCreateWithNativeHandle, NativeHandle,
               ContextImpl->getUrHandleRef(), UrProgram, &Properties,
               &UrKernel);

  if (Backend == backend::opencl)
    Plugin->call(urKernelRetain, UrKernel);

  // Construct the SYCL queue from PI queue.
  return detail::createSyclObjFromImpl<kernel>(
      std::make_shared<kernel_impl>(UrKernel, ContextImpl, KernelBundleImpl));
}

kernel make_kernel(ur_native_handle_t NativeHandle,
                   const context &TargetContext, backend Backend) {
  return make_kernel(
      TargetContext,
      get_empty_interop_kernel_bundle<bundle_state::executable>(TargetContext),
      NativeHandle, false, Backend);
}

} // namespace detail
} // namespace _V1
} // namespace sycl

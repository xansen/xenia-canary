/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/string_util.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_content_device.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/xbox.h"

DEFINE_int32(
    license_mask, 0,
    "Set license mask for activated content.\n"
    " 0 = No licenses enabled.\n"
    " 1 = First license enabled. Generally the full version license in\n"
    "     Xbox Live Arcade titles.\n"
    " -1 or 0xFFFFFFFF = All possible licenses enabled. Generally a\n"
    "                    bad idea, could lead to undefined behavior.",
    "Content");

namespace xe {
namespace kernel {
namespace xam {

dword_result_t XamContentGetLicenseMask_entry(lpdword_t mask_ptr,
                                              lpunknown_t overlapped_ptr) {
  // Each bit in the mask represents a granted license. Available licenses
  // seems to vary from game to game, but most appear to use bit 0 to indicate
  // if the game is purchased or not.
  *mask_ptr = static_cast<uint32_t>(cvars::license_mask);

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  } else {
    return X_ERROR_SUCCESS;
  }
}
DECLARE_XAM_EXPORT2(XamContentGetLicenseMask, kContent, kStub, kHighFrequency);

dword_result_t XamContentResolve_entry(dword_t user_index,
                                       lpvoid_t content_data_ptr,
                                       lpunknown_t buffer_ptr,
                                       dword_t buffer_size, dword_t unk1,
                                       dword_t unk2, dword_t unk3) {
  auto content_data = content_data_ptr.as<XCONTENT_DATA*>();

  // Result of buffer_ptr is sent to RtlInitAnsiString.
  // buffer_size is usually 260 (max path).
  // Games expect zero if resolve was successful.
  assert_always();
  XELOGW("XamContentResolve unimplemented!");
  return X_ERROR_NOT_FOUND;
}
DECLARE_XAM_EXPORT1(XamContentResolve, kContent, kStub);

// https://github.com/MrColdbird/gameservice/blob/master/ContentManager.cpp
// https://github.com/LestaD/SourceEngine2007/blob/master/se2007/engine/xboxsystem.cpp#L499
dword_result_t XamContentCreateEnumerator_entry(
    dword_t user_index, dword_t device_id, dword_t content_type,
    dword_t content_flags, dword_t items_per_enumerate,
    lpdword_t buffer_size_ptr, lpdword_t handle_out) {
  assert_not_null(handle_out);

  auto device_info = device_id == 0 ? nullptr : GetDummyDeviceInfo(device_id);
  if ((device_id && device_info == nullptr) || !handle_out) {
    if (buffer_size_ptr) {
      *buffer_size_ptr = 0;
    }

    // TODO(benvanik): memset 0 the data?
    return X_E_INVALIDARG;
  }

  if (buffer_size_ptr) {
    *buffer_size_ptr = sizeof(XCONTENT_DATA) * items_per_enumerate;
  }

  auto e = make_object<XStaticEnumerator<XCONTENT_DATA>>(kernel_state(),
                                                         items_per_enumerate);
  auto result = e->Initialize(0xFF, 0xFE, 0x20005, 0x20007, 0);
  if (XFAILED(result)) {
    return result;
  }

  if (!device_info || device_info->device_id == DummyDeviceId::HDD) {
    // Get all content data.
    auto content_datas = kernel_state()->content_manager()->ListContent(
        static_cast<uint32_t>(DummyDeviceId::HDD),
        XContentType(uint32_t(content_type)));
    for (const auto& content_data : content_datas) {
      auto item = e->AppendItem();
      *item = content_data;
    }
  }

  if (!device_info || device_info->device_id == DummyDeviceId::ODD) {
    // TODO(gibbed): disc drive content
  }

  XELOGD("XamContentCreateEnumerator: added {} items to enumerator",
         e->item_count());

  *handle_out = e->handle();
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamContentCreateEnumerator, kContent, kImplemented);

enum class kDispositionState : uint32_t { Unknown = 0, Create = 1, Open = 2 };

dword_result_t xeXamContentCreate(dword_t user_index, lpstring_t root_name,
                                  lpvoid_t content_data_ptr,
                                  dword_t content_data_size, dword_t flags,
                                  lpdword_t disposition_ptr,
                                  lpdword_t license_mask_ptr,
                                  dword_t cache_size, qword_t content_size,
                                  lpvoid_t overlapped_ptr) {
  XCONTENT_AGGREGATE_DATA content_data;
  if (content_data_size == sizeof(XCONTENT_DATA)) {
    content_data = *content_data_ptr.as<XCONTENT_DATA*>();
  } else if (content_data_size == sizeof(XCONTENT_AGGREGATE_DATA)) {
    content_data = *content_data_ptr.as<XCONTENT_AGGREGATE_DATA*>();
  } else {
    assert_always();
    return X_ERROR_INVALID_PARAMETER;
  }

  auto content_manager = kernel_state()->content_manager();

  if (overlapped_ptr && disposition_ptr) {
    *disposition_ptr = 0;
  }

  auto run = [content_manager, root_name = root_name.value(), flags,
              content_data, disposition_ptr, license_mask_ptr, overlapped_ptr](
                 uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    X_RESULT result = X_ERROR_INVALID_PARAMETER;
    kDispositionState disposition = kDispositionState::Unknown;
    switch (flags & 0xF) {
      case 1:  // CREATE_NEW
               // Fail if exists.
        if (content_manager->ContentExists(content_data)) {
          result = X_ERROR_ALREADY_EXISTS;
        } else {
          disposition = kDispositionState::Create;
        }
        break;
      case 2:  // CREATE_ALWAYS
               // Overwrite existing, if any.
        if (content_manager->ContentExists(content_data)) {
          content_manager->DeleteContent(content_data);
        }
        disposition = kDispositionState::Create;
        break;
      case 3:  // OPEN_EXISTING
               // Open only if exists.
        if (!content_manager->ContentExists(content_data)) {
          result = X_ERROR_PATH_NOT_FOUND;
        } else {
          disposition = kDispositionState::Open;
        }
        break;
      case 4:  // OPEN_ALWAYS
               // Create if needed.
        if (!content_manager->ContentExists(content_data)) {
          disposition = kDispositionState::Create;
        } else {
          disposition = kDispositionState::Open;
        }
        break;
      case 5:  // TRUNCATE_EXISTING
               // Fail if doesn't exist, if does exist delete and recreate.
        if (!content_manager->ContentExists(content_data)) {
          result = X_ERROR_PATH_NOT_FOUND;
        } else {
          content_manager->DeleteContent(content_data);
          disposition = kDispositionState::Create;
        }
        break;
      default:
        assert_unhandled_case(flags & 0xF);
        break;
    }

    if (disposition == kDispositionState::Create) {
      result = content_manager->CreateContent(root_name, content_data);
      if (XSUCCEEDED(result)) {
        content_manager->WriteContentHeaderFile(&content_data);
      }
    } else if (disposition == kDispositionState::Open) {
      result = content_manager->OpenContent(root_name, content_data);
    }

    if (license_mask_ptr && XSUCCEEDED(result)) {
      *license_mask_ptr = 0;  // Stub!

      // Set license only for DLCs
      if (content_data.content_type == xe::XContentType::kMarketplaceContent) {
        *license_mask_ptr = static_cast<uint32_t>(cvars::license_mask);
      }
    }

    extended_error = X_HRESULT_FROM_WIN32(result);
    length = static_cast<uint32_t>(disposition);

    if (disposition_ptr) {
      *disposition_ptr = static_cast<uint32_t>(disposition);
    }

    if (result && overlapped_ptr) {
      result = X_ERROR_FUNCTION_FAILED;
    }
    return result;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    return run(extended_error, length);
  } else {
    kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
    return X_ERROR_IO_PENDING;
  }
}

dword_result_t XamContentCreateEx_entry(
    dword_t user_index, lpstring_t root_name, lpvoid_t content_data_ptr,
    dword_t flags, lpdword_t disposition_ptr, lpdword_t license_mask_ptr,
    dword_t cache_size, qword_t content_size, lpvoid_t overlapped_ptr) {
  return xeXamContentCreate(user_index, root_name, content_data_ptr,
                            sizeof(XCONTENT_DATA), flags, disposition_ptr,
                            license_mask_ptr, cache_size, content_size,
                            overlapped_ptr);
}
DECLARE_XAM_EXPORT1(XamContentCreateEx, kContent, kImplemented);

dword_result_t XamContentCreate_entry(dword_t user_index, lpstring_t root_name,
                                      lpvoid_t content_data_ptr, dword_t flags,
                                      lpdword_t disposition_ptr,
                                      lpdword_t license_mask_ptr,
                                      lpvoid_t overlapped_ptr) {
  return xeXamContentCreate(user_index, root_name, content_data_ptr,
                            sizeof(XCONTENT_DATA), flags, disposition_ptr,
                            license_mask_ptr, 0, 0, overlapped_ptr);
}
DECLARE_XAM_EXPORT1(XamContentCreate, kContent, kImplemented);

dword_result_t XamContentCreateInternal_entry(
    lpstring_t root_name, lpvoid_t content_data_ptr, dword_t flags,
    lpdword_t disposition_ptr, lpdword_t license_mask_ptr, dword_t cache_size,
    qword_t content_size, lpvoid_t overlapped_ptr) {
  return xeXamContentCreate(0xFE, root_name, content_data_ptr,
                            sizeof(XCONTENT_AGGREGATE_DATA), flags,
                            disposition_ptr, license_mask_ptr, cache_size,
                            content_size, overlapped_ptr);
}
DECLARE_XAM_EXPORT1(XamContentCreateInternal, kContent, kImplemented);

dword_result_t XamContentOpenFile_entry(dword_t user_index,
                                        lpstring_t root_name, lpstring_t path,
                                        dword_t flags,
                                        lpdword_t disposition_ptr,
                                        lpdword_t license_mask_ptr,
                                        lpvoid_t overlapped_ptr) {
  // TODO(gibbed): arguments assumed based on XamContentCreate.
  return X_ERROR_FILE_NOT_FOUND;
}
DECLARE_XAM_EXPORT1(XamContentOpenFile, kContent, kStub);

dword_result_t XamContentFlush_entry(lpstring_t root_name,
                                     lpunknown_t overlapped_ptr) {
  X_RESULT result = X_ERROR_SUCCESS;
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}
DECLARE_XAM_EXPORT1(XamContentFlush, kContent, kStub);

dword_result_t XamContentClose_entry(lpstring_t root_name,
                                     lpunknown_t overlapped_ptr) {
  // Closes a previously opened root from XamContentCreate*.
  auto result =
      kernel_state()->content_manager()->CloseContent(root_name.value());

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}
DECLARE_XAM_EXPORT1(XamContentClose, kContent, kImplemented);

dword_result_t XamContentGetCreator_entry(dword_t user_index,
                                          lpvoid_t content_data_ptr,
                                          lpdword_t is_creator_ptr,
                                          lpqword_t creator_xuid_ptr,
                                          lpunknown_t overlapped_ptr) {
  auto result = X_ERROR_SUCCESS;

  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_DATA*>();

  bool content_exists =
      kernel_state()->content_manager()->ContentExists(content_data);

  if (content_exists) {
    if (content_data.content_type == XContentType::kSavedGame) {
      // User always creates saves.
      *is_creator_ptr = 1;
      if (creator_xuid_ptr) {
        if (kernel_state()->IsUserSignedIn(user_index)) {
          const auto& user_profile = kernel_state()->user_profile(user_index);
          *creator_xuid_ptr = user_profile->xuid();
        } else {
          result = X_ERROR_NO_SUCH_USER;
        }
      }
    } else {
      *is_creator_ptr = 0;
      if (creator_xuid_ptr) {
        *creator_xuid_ptr = 0;
      }
    }
  } else {
    result = X_ERROR_PATH_NOT_FOUND;
  }

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}
DECLARE_XAM_EXPORT1(XamContentGetCreator, kContent, kImplemented);

dword_result_t XamContentGetThumbnail_entry(dword_t user_index,
                                            lpvoid_t content_data_ptr,
                                            lpvoid_t buffer_ptr,
                                            lpdword_t buffer_size_ptr,
                                            lpunknown_t overlapped_ptr) {
  assert_not_null(buffer_size_ptr);
  uint32_t buffer_size = *buffer_size_ptr;
  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_DATA*>();

  // Get thumbnail (if it exists).
  std::vector<uint8_t> buffer;
  auto result = kernel_state()->content_manager()->GetContentThumbnail(
      content_data, &buffer);

  *buffer_size_ptr = uint32_t(buffer.size());

  if (XSUCCEEDED(result)) {
    // Write data, if we were given a pointer.
    // This may have just been a size query.
    if (buffer_ptr) {
      if (buffer_size < buffer.size()) {
        // Dest buffer too small.
        result = X_ERROR_INSUFFICIENT_BUFFER;
      } else {
        // Copy data.
        std::memcpy((uint8_t*)buffer_ptr, buffer.data(), buffer.size());
      }
    }
  }

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}
DECLARE_XAM_EXPORT1(XamContentGetThumbnail, kContent, kImplemented);

dword_result_t XamContentSetThumbnail_entry(dword_t user_index,
                                            lpvoid_t content_data_ptr,
                                            lpvoid_t buffer_ptr,
                                            dword_t buffer_size,
                                            lpunknown_t overlapped_ptr) {
  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_DATA*>();

  // Buffer is PNG data.
  auto buffer = std::vector<uint8_t>((uint8_t*)buffer_ptr,
                                     (uint8_t*)buffer_ptr + buffer_size);
  auto result = kernel_state()->content_manager()->SetContentThumbnail(
      content_data, std::move(buffer));

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}
DECLARE_XAM_EXPORT1(XamContentSetThumbnail, kContent, kImplemented);

dword_result_t XamContentDelete_entry(dword_t user_index,
                                      lpvoid_t content_data_ptr,
                                      lpunknown_t overlapped_ptr) {
  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_DATA*>();

  auto result = kernel_state()->content_manager()->DeleteContent(content_data);

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}
DECLARE_XAM_EXPORT1(XamContentDelete, kContent, kImplemented);

dword_result_t XamContentDeleteInternal_entry(lpvoid_t content_data_ptr,
                                              lpunknown_t overlapped_ptr) {
  // INFO: Analysis of xam.xex shows that "internal" functions are wrappers with
  // 0xFE as user_index
  return XamContentDelete_entry(0xFE, content_data_ptr, overlapped_ptr);
}
DECLARE_XAM_EXPORT1(XamContentDeleteInternal, kContent, kImplemented);

typedef struct {
  xe::be<uint32_t> stringTitlePtr;
  xe::be<uint32_t> stringTextPtr;
  xe::be<uint32_t> stringBtnMsgPtr;
} X_SWAPDISC_ERROR_MESSAGE;
static_assert_size(X_SWAPDISC_ERROR_MESSAGE, 12);

dword_result_t XamSwapDisc_entry(
    dword_t disc_number, pointer_t<X_KEVENT> completion_handle,
    pointer_t<X_SWAPDISC_ERROR_MESSAGE> error_message) {
  xex2_opt_execution_info* info = nullptr;
  kernel_state()->GetExecutableModule()->GetOptHeader(XEX_HEADER_EXECUTION_INFO,
                                                      &info);

  if (info->disc_number > info->disc_count) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto completion_event = [completion_handle]() -> void {
    auto kevent = xboxkrnl::xeKeSetEvent(completion_handle, 1, 0);

    // Release the completion handle
    auto object =
        XObject::GetNativeObject<XObject>(kernel_state(), completion_handle);
    if (object) {
      object->Retain();
    }
  };

  if (info->disc_number == disc_number) {
    completion_event();
    return X_ERROR_SUCCESS;
  }

  auto filesystem = kernel_state()->file_system();
  auto mount_path = "\\Device\\LauncherData";

  if (filesystem->ResolvePath(mount_path) != NULL) {
    filesystem->UnregisterDevice(mount_path);
  }

  std::u16string text_message = xe::load_and_swap<std::u16string>(
      kernel_state()->memory()->TranslateVirtual(error_message->stringTextPtr));

  const std::filesystem::path new_disc_path =
      kernel_state()->emulator()->GetNewDiscPath(xe::to_utf8(text_message));
  XELOGI("GetNewDiscPath returned path {}.", new_disc_path.string().c_str());

  // TODO(Gliniak): Implement checking if inserted file is requested one
  kernel_state()->emulator()->MountPath(new_disc_path, mount_path);
  completion_event();

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamSwapDisc, kContent, kSketchy);

dword_result_t XamLoaderGetMediaInfoEx_entry(dword_t unk1, dword_t unk2,
                                             lpdword_t unk3) {
  *unk3 = 0;
  return 0;
}

DECLARE_XAM_EXPORT1(XamLoaderGetMediaInfoEx, kContent, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Content);

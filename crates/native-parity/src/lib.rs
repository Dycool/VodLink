#![deny(unsafe_op_in_unsafe_fn)]

#[cfg(target_os = "windows")]
mod windows {
    use std::collections::BTreeSet;
    use std::ffi::OsString;
    use std::os::windows::ffi::OsStringExt;
    use std::path::PathBuf;
    use windows_sys::Win32::Foundation::{CloseHandle, FALSE, HWND, LPARAM};
    use windows_sys::Win32::System::Threading::{
        OpenProcess, PROCESS_NAME_WIN32, PROCESS_QUERY_LIMITED_INFORMATION,
        QueryFullProcessImageNameW,
    };
    use windows_sys::Win32::UI::WindowsAndMessaging::{
        EnumWindows, GWL_EXSTYLE, GW_OWNER, GetWindow, GetWindowLongW, GetWindowTextLengthW,
        GetWindowThreadProcessId, IsWindowVisible, WS_EX_TOOLWINDOW,
    };

    pub(super) fn windowed_process_names() -> Vec<String> {
        let mut pids = BTreeSet::<u32>::new();
        let context = (&mut pids as *mut BTreeSet<u32>) as LPARAM;
        // SAFETY: EnumWindows invokes the callback synchronously before returning. `context`
        // points to `pids`, which remains alive and exclusively owned for that entire call.
        unsafe {
            EnumWindows(Some(collect_windowed_pid), context);
        }

        let mut names = BTreeSet::<String>::new();
        for pid in pids {
            if let Some(name) = process_image_basename(pid) {
                names.insert(name);
            }
        }
        names.into_iter().collect()
    }

    unsafe extern "system" fn collect_windowed_pid(hwnd: HWND, lparam: LPARAM) -> i32 {
        // SAFETY: `hwnd` is supplied by EnumWindows and is valid for these read-only queries
        // during the callback. No returned handle is retained beyond this invocation.
        let eligible = unsafe {
            IsWindowVisible(hwnd) != FALSE
                && GetWindow(hwnd, GW_OWNER).is_null()
                && GetWindowTextLengthW(hwnd) > 0
                && (GetWindowLongW(hwnd, GWL_EXSTYLE) as u32 & WS_EX_TOOLWINDOW) == 0
        };
        if !eligible {
            return 1;
        }

        let mut pid = 0_u32;
        // SAFETY: `pid` is a valid writable u32 for the duration of the call and `hwnd`
        // originates from EnumWindows.
        unsafe {
            GetWindowThreadProcessId(hwnd, &mut pid);
        }
        if pid != 0 {
            // SAFETY: `lparam` is exactly the pointer to the live BTreeSet passed by
            // `windowed_process_names`; EnumWindows is synchronous and does not retain it.
            unsafe {
                let pids = &mut *(lparam as *mut BTreeSet<u32>);
                pids.insert(pid);
            }
        }
        1
    }

    fn process_image_basename(pid: u32) -> Option<String> {
        // SAFETY: OpenProcess receives a PID discovered from a live top-level window and
        // requests query-only access; the returned handle is closed on every success path.
        let process = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) };
        if process.is_null() {
            return None;
        }

        let mut buffer = vec![0_u16; 32_768];
        let mut size = buffer.len() as u32;
        // SAFETY: `buffer` has capacity for `size` UTF-16 code units, `size` is writable,
        // and `process` is a valid query handle until CloseHandle below.
        let succeeded = unsafe {
            QueryFullProcessImageNameW(
                process,
                PROCESS_NAME_WIN32,
                buffer.as_mut_ptr(),
                &mut size,
            )
        } != FALSE;
        // SAFETY: `process` was returned by OpenProcess above and has not been closed yet.
        unsafe {
            CloseHandle(process);
        }
        if !succeeded || size == 0 {
            return None;
        }

        let path = PathBuf::from(OsString::from_wide(&buffer[..size as usize]));
        path.file_name()
            .map(|name| name.to_string_lossy().trim().to_lowercase())
            .filter(|name| !name.is_empty())
    }
}

#[cfg(target_os = "windows")]
pub fn windowed_process_names() -> Vec<String> {
    windows::windowed_process_names()
}

#[cfg(not(target_os = "windows"))]
pub fn windowed_process_names() -> Vec<String> {
    Vec::new()
}

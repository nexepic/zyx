use std::ffi::CString;
use std::path::Path;
use std::ptr;

#[cfg(unix)]
use std::os::unix::ffi::OsStrExt;

use zyxdb_sys as sys;

use crate::error::status_to_result;
use crate::{Params, ResultSet};

pub struct Database {
    raw: *mut sys::zyx_driver_db_t,
}

impl Database {
    pub fn open(path: impl AsRef<Path>) -> crate::Result<Self> {
        let path = path_to_cstring(path.as_ref())?;
        let mut raw = ptr::null_mut();
        let mut error = ptr::null_mut();
        let status = unsafe { sys::zyx_driver_db_open(path.as_ptr(), &mut raw, &mut error) };
        status_to_result(status, error)?;
        Ok(Self { raw })
    }

    pub fn execute(&self, query: &str, params: Option<&Params>) -> crate::Result<ResultSet> {
        let query = CString::new(query)?;
        let mut raw_result = ptr::null_mut();
        let mut error = ptr::null_mut();
        let raw_params = params.map_or(ptr::null_mut(), |params| params.raw);
        let status = unsafe {
            sys::zyx_driver_db_execute(
                self.raw,
                query.as_ptr(),
                raw_params,
                &mut raw_result,
                &mut error,
            )
        };
        status_to_result(status, error)?;
        Ok(unsafe { ResultSet::from_raw(raw_result) })
    }
}

impl Drop for Database {
    fn drop(&mut self) {
        if !self.raw.is_null() {
            let mut error = ptr::null_mut();
            unsafe {
                let _ = sys::zyx_driver_db_close(self.raw, &mut error);
                if !error.is_null() {
                    sys::zyx_driver_error_free(error);
                }
            }
        }
    }
}

#[cfg(unix)]
fn path_to_cstring(path: &Path) -> crate::Result<CString> {
    std::str::from_utf8(path.as_os_str().as_bytes()).map_err(|_| {
        crate::Error::new(
            crate::ErrorCode::InvalidArgument,
            "path must be valid UTF-8",
        )
    })?;
    Ok(CString::new(path.as_os_str().as_bytes())?)
}

#[cfg(not(unix))]
fn path_to_cstring(path: &Path) -> crate::Result<CString> {
    let path = path.to_str().ok_or_else(|| {
        crate::Error::new(
            crate::ErrorCode::InvalidArgument,
            "path must be valid UTF-8",
        )
    })?;
    Ok(CString::new(path.as_bytes())?)
}

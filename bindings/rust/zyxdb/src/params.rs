use std::ffi::CString;
use std::ptr;

use zyxdb_sys as sys;

use crate::error::status_to_result;

pub struct Params {
    pub(crate) raw: *mut sys::zyx_driver_params_t,
}

impl Params {
    pub fn new() -> Self {
        let mut raw = ptr::null_mut();
        let mut error = ptr::null_mut();
        let status = unsafe { sys::zyx_driver_params_create(&mut raw, &mut error) };
        if status != sys::ZYX_DRIVER_OK {
            let err = unsafe { crate::Error::from_abi(status, error) };
            panic!("failed to create ZYX params: {err}");
        }
        Self { raw }
    }

    pub fn set<V: IntoParam>(self, key: &str, value: V) -> crate::Result<Self> {
        value.set_param(&self, key)?;
        Ok(self)
    }
}

impl Default for Params {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for Params {
    fn drop(&mut self) {
        if !self.raw.is_null() {
            let mut error = ptr::null_mut();
            unsafe {
                sys::zyx_driver_params_free(self.raw, &mut error);
                if !error.is_null() {
                    sys::zyx_driver_error_free(error);
                }
            }
        }
    }
}

pub trait IntoParam {
    fn set_param(self, params: &Params, key: &str) -> crate::Result<()>;
}

fn key_cstring(key: &str) -> crate::Result<CString> {
    CString::new(key).map_err(Into::into)
}

impl IntoParam for i64 {
    fn set_param(self, params: &Params, key: &str) -> crate::Result<()> {
        let key = key_cstring(key)?;
        let mut error = ptr::null_mut();
        let status =
            unsafe { sys::zyx_driver_params_set_int64(params.raw, key.as_ptr(), self, &mut error) };
        status_to_result(status, error)
    }
}

impl IntoParam for i32 {
    fn set_param(self, params: &Params, key: &str) -> crate::Result<()> {
        i64::from(self).set_param(params, key)
    }
}

impl IntoParam for f64 {
    fn set_param(self, params: &Params, key: &str) -> crate::Result<()> {
        let key = key_cstring(key)?;
        let mut error = ptr::null_mut();
        let status = unsafe {
            sys::zyx_driver_params_set_double(params.raw, key.as_ptr(), self, &mut error)
        };
        status_to_result(status, error)
    }
}

impl IntoParam for bool {
    fn set_param(self, params: &Params, key: &str) -> crate::Result<()> {
        let key = key_cstring(key)?;
        let mut error = ptr::null_mut();
        let status =
            unsafe { sys::zyx_driver_params_set_bool(params.raw, key.as_ptr(), self, &mut error) };
        status_to_result(status, error)
    }
}

impl IntoParam for &str {
    fn set_param(self, params: &Params, key: &str) -> crate::Result<()> {
        let key = key_cstring(key)?;
        let value = CString::new(self)?;
        let mut error = ptr::null_mut();
        let status = unsafe {
            sys::zyx_driver_params_set_string(params.raw, key.as_ptr(), value.as_ptr(), &mut error)
        };
        status_to_result(status, error)
    }
}

impl IntoParam for String {
    fn set_param(self, params: &Params, key: &str) -> crate::Result<()> {
        self.as_str().set_param(params, key)
    }
}

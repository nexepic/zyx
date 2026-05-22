use std::ffi::CStr;
use std::marker::PhantomData;
use std::ptr;

use zyxdb_sys as sys;

use crate::error::{status_to_result, ErrorCode};
use crate::Error;

pub struct ResultSet {
    raw: *mut sys::zyx_driver_result_t,
}

impl ResultSet {
    pub(crate) unsafe fn from_raw(raw: *mut sys::zyx_driver_result_t) -> Self {
        Self { raw }
    }

    pub fn next(&mut self) -> crate::Result<Option<Record<'_>>> {
        let mut error = ptr::null_mut();
        let status = unsafe { sys::zyx_driver_result_next(self.raw, &mut error) };
        match status {
            sys::ZYX_DRIVER_ROW => Ok(Some(Record {
                raw: self.raw,
                _lifetime: PhantomData,
            })),
            sys::ZYX_DRIVER_DONE => Ok(None),
            _ => Err(unsafe { Error::from_abi(status, error) }),
        }
    }

    pub fn column_count(&self) -> usize {
        unsafe { sys::zyx_driver_result_column_count(self.raw) as usize }
    }

    pub fn column_name(&mut self, index: usize) -> crate::Result<&str> {
        let index = checked_index(index)?;
        let ptr = unsafe { sys::zyx_driver_result_column_name(self.raw, index) };
        if ptr.is_null() {
            return Err(Error::new(
                ErrorCode::OutOfRange,
                "column index is out of range",
            ));
        }
        Ok(unsafe { CStr::from_ptr(ptr) }
            .to_str()
            .map_err(|err| Error::new(ErrorCode::InternalError, err.to_string()))?)
    }
}

impl Drop for ResultSet {
    fn drop(&mut self) {
        if !self.raw.is_null() {
            unsafe { sys::zyx_driver_result_free(self.raw) }
        }
    }
}

pub struct Record<'a> {
    raw: *mut sys::zyx_driver_result_t,
    _lifetime: PhantomData<&'a mut ResultSet>,
}

impl<'a> Record<'a> {
    pub fn get_i64(&self, index: usize) -> crate::Result<i64> {
        let mut value = 0;
        let mut error = ptr::null_mut();
        let index = checked_index(index)?;
        let status =
            unsafe { sys::zyx_driver_result_get_int64(self.raw, index, &mut value, &mut error) };
        status_to_result(status, error)?;
        Ok(value)
    }

    pub fn get_f64(&self, index: usize) -> crate::Result<f64> {
        let mut value = 0.0;
        let mut error = ptr::null_mut();
        let index = checked_index(index)?;
        let status =
            unsafe { sys::zyx_driver_result_get_double(self.raw, index, &mut value, &mut error) };
        status_to_result(status, error)?;
        Ok(value)
    }

    pub fn get_bool(&self, index: usize) -> crate::Result<bool> {
        let mut value = false;
        let mut error = ptr::null_mut();
        let index = checked_index(index)?;
        let status =
            unsafe { sys::zyx_driver_result_get_bool(self.raw, index, &mut value, &mut error) };
        status_to_result(status, error)?;
        Ok(value)
    }

    pub fn get_str(&self, index: usize) -> crate::Result<String> {
        let mut value = ptr::null();
        let mut error = ptr::null_mut();
        let index = checked_index(index)?;
        let status =
            unsafe { sys::zyx_driver_result_get_string(self.raw, index, &mut value, &mut error) };
        status_to_result(status, error)?;
        if value.is_null() {
            return Err(Error::new(
                ErrorCode::InternalError,
                "driver returned a null string",
            ));
        }
        Ok(unsafe { CStr::from_ptr(value) }
            .to_str()
            .map_err(|err| Error::new(ErrorCode::InternalError, err.to_string()))?
            .to_owned())
    }
}

fn checked_index(index: usize) -> crate::Result<u32> {
    let index = i32::try_from(index).map_err(|_| {
        Error::new(
            ErrorCode::OutOfRange,
            "column index exceeds driver ABI range",
        )
    })?;
    Ok(index as u32)
}

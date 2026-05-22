use std::ffi::CStr;
use std::fmt;

use zyxdb_sys as sys;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ErrorCode {
    InvalidArgument,
    NotFound,
    OpenFailed,
    ParseError,
    ExecutionError,
    TransactionError,
    ReadOnlyViolation,
    TypeMismatch,
    OutOfRange,
    IoError,
    OutOfMemory,
    InternalError,
    Unknown(i32),
}

impl ErrorCode {
    pub(crate) fn from_status(status: sys::zyx_driver_status_t) -> Self {
        match status {
            sys::ZYX_DRIVER_INVALID_ARGUMENT => Self::InvalidArgument,
            sys::ZYX_DRIVER_NOT_FOUND => Self::NotFound,
            sys::ZYX_DRIVER_OPEN_FAILED => Self::OpenFailed,
            sys::ZYX_DRIVER_PARSE_ERROR => Self::ParseError,
            sys::ZYX_DRIVER_EXECUTION_ERROR => Self::ExecutionError,
            sys::ZYX_DRIVER_TRANSACTION_ERROR => Self::TransactionError,
            sys::ZYX_DRIVER_READ_ONLY_VIOLATION => Self::ReadOnlyViolation,
            sys::ZYX_DRIVER_TYPE_MISMATCH => Self::TypeMismatch,
            sys::ZYX_DRIVER_OUT_OF_RANGE => Self::OutOfRange,
            sys::ZYX_DRIVER_IO_ERROR => Self::IoError,
            sys::ZYX_DRIVER_OUT_OF_MEMORY => Self::OutOfMemory,
            sys::ZYX_DRIVER_INTERNAL_ERROR => Self::InternalError,
            other => Self::Unknown(other),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error {
    code: ErrorCode,
    message: String,
}

impl Error {
    pub(crate) fn new(code: ErrorCode, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
        }
    }

    pub(crate) unsafe fn from_abi(
        status: sys::zyx_driver_status_t,
        error: *mut sys::zyx_driver_error_t,
    ) -> Self {
        if error.is_null() {
            return Self::new(
                ErrorCode::from_status(status),
                format!("ZYX driver error status {status}"),
            );
        }

        let code = ErrorCode::from_status(sys::zyx_driver_error_code(error));
        let message_ptr = sys::zyx_driver_error_message(error);
        let message = if message_ptr.is_null() {
            format!("ZYX driver error status {status}")
        } else {
            CStr::from_ptr(message_ptr).to_string_lossy().into_owned()
        };
        sys::zyx_driver_error_free(error);
        Self::new(code, message)
    }

    pub fn code(&self) -> ErrorCode {
        self.code
    }

    pub fn message(&self) -> &str {
        &self.message
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:?}: {}", self.code, self.message)
    }
}

impl std::error::Error for Error {}

impl From<std::ffi::NulError> for Error {
    fn from(error: std::ffi::NulError) -> Self {
        Self::new(ErrorCode::InvalidArgument, error.to_string())
    }
}

pub(crate) fn status_to_result(
    status: sys::zyx_driver_status_t,
    error: *mut sys::zyx_driver_error_t,
) -> crate::Result<()> {
    if status == sys::ZYX_DRIVER_OK {
        Ok(())
    } else {
        Err(unsafe { Error::from_abi(status, error) })
    }
}

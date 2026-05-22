#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(non_upper_case_globals)]

use std::ffi::c_char;
use std::os::raw::{c_double, c_int};

#[repr(C)]
pub struct zyx_driver_db_t {
    _private: [u8; 0],
}

#[repr(C)]
pub struct zyx_driver_txn_t {
    _private: [u8; 0],
}

#[repr(C)]
pub struct zyx_driver_result_t {
    _private: [u8; 0],
}

#[repr(C)]
pub struct zyx_driver_params_t {
    _private: [u8; 0],
}

#[repr(C)]
pub struct zyx_driver_error_t {
    _private: [u8; 0],
}

pub type zyx_driver_status_t = c_int;

pub const ZYX_DRIVER_OK: zyx_driver_status_t = 0;
pub const ZYX_DRIVER_ROW: zyx_driver_status_t = 1;
pub const ZYX_DRIVER_DONE: zyx_driver_status_t = 2;
pub const ZYX_DRIVER_INVALID_ARGUMENT: zyx_driver_status_t = 100;
pub const ZYX_DRIVER_NOT_FOUND: zyx_driver_status_t = 101;
pub const ZYX_DRIVER_OPEN_FAILED: zyx_driver_status_t = 102;
pub const ZYX_DRIVER_PARSE_ERROR: zyx_driver_status_t = 200;
pub const ZYX_DRIVER_EXECUTION_ERROR: zyx_driver_status_t = 201;
pub const ZYX_DRIVER_TRANSACTION_ERROR: zyx_driver_status_t = 300;
pub const ZYX_DRIVER_READ_ONLY_VIOLATION: zyx_driver_status_t = 301;
pub const ZYX_DRIVER_TYPE_MISMATCH: zyx_driver_status_t = 400;
pub const ZYX_DRIVER_OUT_OF_RANGE: zyx_driver_status_t = 401;
pub const ZYX_DRIVER_IO_ERROR: zyx_driver_status_t = 500;
pub const ZYX_DRIVER_OUT_OF_MEMORY: zyx_driver_status_t = 600;
pub const ZYX_DRIVER_INTERNAL_ERROR: zyx_driver_status_t = 900;

pub type zyx_driver_value_type_t = c_int;

pub const ZYX_DRIVER_VALUE_NULL: zyx_driver_value_type_t = 0;
pub const ZYX_DRIVER_VALUE_BOOL: zyx_driver_value_type_t = 1;
pub const ZYX_DRIVER_VALUE_INT64: zyx_driver_value_type_t = 2;
pub const ZYX_DRIVER_VALUE_DOUBLE: zyx_driver_value_type_t = 3;
pub const ZYX_DRIVER_VALUE_STRING: zyx_driver_value_type_t = 4;
pub const ZYX_DRIVER_VALUE_NODE: zyx_driver_value_type_t = 5;
pub const ZYX_DRIVER_VALUE_EDGE: zyx_driver_value_type_t = 6;
pub const ZYX_DRIVER_VALUE_LIST: zyx_driver_value_type_t = 7;
pub const ZYX_DRIVER_VALUE_MAP: zyx_driver_value_type_t = 8;

extern "C" {
    pub fn zyx_driver_abi_version_major() -> u32;
    pub fn zyx_driver_abi_version_minor() -> u32;
    pub fn zyx_driver_abi_version_patch() -> u32;
    pub fn zyx_driver_runtime_version() -> *const c_char;

    pub fn zyx_driver_error_code(error: *const zyx_driver_error_t) -> zyx_driver_status_t;
    pub fn zyx_driver_error_message(error: *const zyx_driver_error_t) -> *const c_char;
    pub fn zyx_driver_error_free(error: *mut zyx_driver_error_t);

    pub fn zyx_driver_db_open(
        path: *const c_char,
        out_db: *mut *mut zyx_driver_db_t,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
    pub fn zyx_driver_db_open_if_exists(
        path: *const c_char,
        out_db: *mut *mut zyx_driver_db_t,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
    pub fn zyx_driver_db_close(
        db: *mut zyx_driver_db_t,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;

    pub fn zyx_driver_txn_begin(
        db: *mut zyx_driver_db_t,
        out_txn: *mut *mut zyx_driver_txn_t,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
    pub fn zyx_driver_txn_begin_read_only(
        db: *mut zyx_driver_db_t,
        out_txn: *mut *mut zyx_driver_txn_t,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
    pub fn zyx_driver_txn_execute(
        txn: *mut zyx_driver_txn_t,
        cypher: *const c_char,
        params: *const zyx_driver_params_t,
        out_result: *mut *mut zyx_driver_result_t,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
    pub fn zyx_driver_txn_commit(
        txn: *mut zyx_driver_txn_t,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
    pub fn zyx_driver_txn_rollback(
        txn: *mut zyx_driver_txn_t,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
    pub fn zyx_driver_txn_close(
        txn: *mut zyx_driver_txn_t,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;

    pub fn zyx_driver_params_create(
        out_params: *mut *mut zyx_driver_params_t,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
    pub fn zyx_driver_params_free(
        params: *mut zyx_driver_params_t,
        out_error: *mut *mut zyx_driver_error_t,
    );
    pub fn zyx_driver_params_set_null(
        params: *mut zyx_driver_params_t,
        key: *const c_char,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
    pub fn zyx_driver_params_set_bool(
        params: *mut zyx_driver_params_t,
        key: *const c_char,
        value: bool,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
    pub fn zyx_driver_params_set_int64(
        params: *mut zyx_driver_params_t,
        key: *const c_char,
        value: i64,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
    pub fn zyx_driver_params_set_double(
        params: *mut zyx_driver_params_t,
        key: *const c_char,
        value: c_double,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
    pub fn zyx_driver_params_set_string(
        params: *mut zyx_driver_params_t,
        key: *const c_char,
        value: *const c_char,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;

    pub fn zyx_driver_db_execute(
        db: *mut zyx_driver_db_t,
        cypher: *const c_char,
        params: *mut zyx_driver_params_t,
        out_result: *mut *mut zyx_driver_result_t,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
    pub fn zyx_driver_result_free(result: *mut zyx_driver_result_t);
    pub fn zyx_driver_result_next(
        result: *mut zyx_driver_result_t,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
    pub fn zyx_driver_result_column_count(result: *const zyx_driver_result_t) -> u32;
    pub fn zyx_driver_result_column_name(
        result: *mut zyx_driver_result_t,
        column: u32,
    ) -> *const c_char;
    pub fn zyx_driver_result_value_type(
        result: *const zyx_driver_result_t,
        column: u32,
    ) -> zyx_driver_value_type_t;
    pub fn zyx_driver_result_get_int64(
        result: *const zyx_driver_result_t,
        column: u32,
        out_value: *mut i64,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
    pub fn zyx_driver_result_get_double(
        result: *const zyx_driver_result_t,
        column: u32,
        out_value: *mut c_double,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
    pub fn zyx_driver_result_get_bool(
        result: *const zyx_driver_result_t,
        column: u32,
        out_value: *mut bool,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
    pub fn zyx_driver_result_get_string(
        result: *mut zyx_driver_result_t,
        column: u32,
        out_value: *mut *const c_char,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
    pub fn zyx_driver_result_get_node_id(
        result: *const zyx_driver_result_t,
        column: u32,
        out_value: *mut i64,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
    pub fn zyx_driver_result_get_node_label_count(
        result: *const zyx_driver_result_t,
        column: u32,
        out_value: *mut u32,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
    pub fn zyx_driver_result_get_node_label(
        result: *mut zyx_driver_result_t,
        column: u32,
        label_index: u32,
        out_value: *mut *const c_char,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
    pub fn zyx_driver_result_get_edge_id(
        result: *const zyx_driver_result_t,
        column: u32,
        out_value: *mut i64,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
    pub fn zyx_driver_result_get_edge_source_id(
        result: *const zyx_driver_result_t,
        column: u32,
        out_value: *mut i64,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
    pub fn zyx_driver_result_get_edge_target_id(
        result: *const zyx_driver_result_t,
        column: u32,
        out_value: *mut i64,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
    pub fn zyx_driver_result_get_edge_type(
        result: *mut zyx_driver_result_t,
        column: u32,
        out_value: *mut *const c_char,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
    pub fn zyx_driver_result_get_entity_properties_json(
        result: *mut zyx_driver_result_t,
        column: u32,
        out_value: *mut *const c_char,
        out_error: *mut *mut zyx_driver_error_t,
    ) -> zyx_driver_status_t;
}

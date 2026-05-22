mod database;
mod error;
mod params;
mod result;

pub use database::Database;
pub use error::{Error, ErrorCode};
pub use params::{IntoParam, Params};
pub use result::{Record, ResultSet};

pub type Result<T> = std::result::Result<T, Error>;

#[macro_export]
macro_rules! params {
    () => {
        $crate::Params::try_new()
    };
    ($($key:expr => $value:expr),+ $(,)?) => {{
        (|| -> $crate::Result<$crate::Params> {
            let params = $crate::Params::try_new()?;
            $(let params = params.set($key, $value)?;)+
            Ok(params)
        })()
    }};
}

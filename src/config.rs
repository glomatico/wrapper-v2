use std::{env, net::IpAddr, path::PathBuf};

use crate::error::ApiError;
use crate::profile::RuntimeProfile;

const DEFAULT_SESSION_DIR: &str = "/data/data/com.apple.android.music/files";

#[derive(Clone)]
pub struct Config {
    pub profile: RuntimeProfile,
    pub host: IpAddr,
    pub port: u16,
    pub launcher: PathBuf,
    pub rootfs: PathBuf,
    pub session_dir: String,
    pub device_info: String,
    pub dns: Vec<String>,
}

impl Config {
    pub fn from_env() -> Result<Self, ApiError> {
        let profile = RuntimeProfile::select(
            &env::var("WRAPPER_ARCH").unwrap_or_else(|_| std::env::consts::ARCH.to_string()),
        )?;
        let host = env::var("WRAPPER_HOST")
            .unwrap_or_else(|_| "127.0.0.1".into())
            .parse()
            .map_err(|_| ApiError::bad_config("WRAPPER_HOST"))?;
        let port = env::var("WRAPPER_PORT")
            .unwrap_or_else(|_| "80".into())
            .parse::<u16>()
            .map_err(|_| ApiError::bad_config("WRAPPER_PORT"))?;
        if port == 0 {
            return Err(ApiError::bad_config("WRAPPER_PORT"));
        }
        let launcher = env::var("WRAPPER_LAUNCHER")
            .unwrap_or_else(|_| format!("./build/{}/host/worker-launch", profile.architecture));
        let rootfs = env::var("WRAPPER_ROOTFS")
            .unwrap_or_else(|_| format!("./build/{}/rootfs", profile.architecture));
        let rootfs =
            std::fs::canonicalize(&rootfs).map_err(|_| ApiError::bad_config("WRAPPER_ROOTFS"))?;
        if !rootfs.is_dir() {
            return Err(ApiError::bad_config("WRAPPER_ROOTFS"));
        }
        profile.validate_rootfs(&rootfs)?;
        let launcher = std::fs::canonicalize(&launcher)
            .map_err(|_| ApiError::bad_config("WRAPPER_LAUNCHER"))?;
        let device_info = env::var("WRAPPER_DEVICE_INFO")
            .map_err(|_| ApiError::bad_config("WRAPPER_DEVICE_INFO"))?;
        if device_info.split('/').count() != 9
            || device_info.split('/').any(str::is_empty)
            || device_info.len() > 512
        {
            return Err(ApiError::bad_config("WRAPPER_DEVICE_INFO"));
        }
        let session_dir =
            env::var("WRAPPER_SESSION_DIR").unwrap_or_else(|_| DEFAULT_SESSION_DIR.into());
        if !session_dir.starts_with('/') || session_dir.contains("/../") || session_dir == "/" {
            return Err(ApiError::bad_config("WRAPPER_SESSION_DIR"));
        }
        let dns: Vec<String> = env::var("WRAPPER_DNS")
            .unwrap_or_else(|_| "1.1.1.1,8.8.8.8".into())
            .split(',')
            .map(str::trim)
            .map(str::to_string)
            .collect();
        if dns.is_empty() || dns.len() > 4 || dns.iter().any(|s| s.parse::<IpAddr>().is_err()) {
            return Err(ApiError::bad_config("WRAPPER_DNS"));
        }
        Ok(Self {
            profile,
            host,
            port,
            launcher,
            rootfs,
            session_dir,
            device_info,
            dns,
        })
    }
}

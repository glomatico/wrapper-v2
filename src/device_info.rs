//! Persisted device identity for setups without an environment file.
//! The native worker independently pins its hash in .worker-device.

use std::{
    fmt::Write as _,
    fs::{self, File, OpenOptions},
    io::{Read, Write},
    os::unix::fs::OpenOptionsExt,
    path::Path,
};

use crate::error::ApiError;

const FILE_NAME: &str = ".wrapper-device-info";
const LEGACY_FILE_NAME: &str = ".worker-device";
const DEVICE_PREFIX: &str = "Music/4.9/Android/10/Samsung S9/7663313/en-US/en-US/";

fn invalid() -> ApiError {
    ApiError::bad_config("WRAPPER_DEVICE_INFO")
}

fn validate(value: &str) -> Result<(), ApiError> {
    if value.len() > 512
        || value.split('/').count() != 9
        || value.split('/').any(str::is_empty)
        || value.chars().any(char::is_control)
    {
        return Err(invalid());
    }
    Ok(())
}

fn random_suffix() -> Result<String, ApiError> {
    let mut random = [0u8; 16];
    File::open("/dev/urandom")
        .and_then(|mut file| file.read_exact(&mut random))
        .map_err(|_| invalid())?;
    let mut suffix = String::with_capacity(32);
    for byte in random {
        write!(suffix, "{byte:02x}").map_err(|_| invalid())?;
    }
    Ok(suffix)
}

fn read_identity(path: &Path) -> Result<String, ApiError> {
    if path
        .symlink_metadata()
        .map_err(|_| invalid())?
        .file_type()
        .is_symlink()
    {
        return Err(invalid());
    }
    let contents = fs::read_to_string(path).map_err(|_| invalid())?;
    let value = contents.strip_suffix('\n').unwrap_or(&contents);
    validate(value)?;
    Ok(value.to_owned())
}

/// Use an explicit identity or atomically generate and persist a new one.
/// Never replace the identity of a legacy session with an unknown old value.
pub fn resolve(
    rootfs: &Path,
    session_dir: &str,
    configured: Option<&str>,
) -> Result<String, ApiError> {
    let directory = rootfs.join(session_dir.trim_start_matches('/'));
    let file = directory.join(FILE_NAME);
    if let Some(value) = configured.filter(|value| !value.is_empty()) {
        validate(value)?;
        if file.symlink_metadata().is_ok() && read_identity(&file)? != value {
            return Err(ApiError::bad_config("WRAPPER_DEVICE_INFO_MISMATCH"));
        }
        return Ok(value.to_owned());
    }

    if file.symlink_metadata().is_ok() {
        return read_identity(&file);
    }
    if directory.join(LEGACY_FILE_NAME).exists() {
        return Err(ApiError::bad_config("WRAPPER_DEVICE_INFO_LEGACY"));
    }
    if !directory.is_dir() {
        return Err(invalid());
    }

    let identity = format!("{DEVICE_PREFIX}{}", random_suffix()?);
    let temp_path = directory.join(format!(".wrapper-device-info-{}.tmp", random_suffix()?));
    let write_result = (|| -> Result<(), ApiError> {
        let mut temp = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(&temp_path)
            .map_err(|_| invalid())?;
        temp.write_all(identity.as_bytes()).map_err(|_| invalid())?;
        temp.write_all(b"\n").map_err(|_| invalid())?;
        temp.sync_all().map_err(|_| invalid())?;
        // A hard link publishes the completed file atomically without replacing
        // an identity created by a concurrent server.
        match fs::hard_link(&temp_path, &file) {
            Ok(()) => Ok(()),
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => Ok(()),
            Err(_) => Err(invalid()),
        }
    })();
    let _ = fs::remove_file(&temp_path);
    write_result?;
    read_identity(&file)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;

    fn directory() -> std::path::PathBuf {
        let base =
            std::env::temp_dir().join(format!("wrapper-identity-{}", random_suffix().unwrap()));
        fs::create_dir(&base).unwrap();
        base
    }

    #[test]
    fn persists_and_reuses_unique_identity() {
        let base = directory();
        let another = directory();
        fs::create_dir(base.join("data")).unwrap();
        let generated = resolve(&base, "/data", None).unwrap();
        assert_eq!(generated.split('/').count(), 9);
        assert_eq!(generated, resolve(&base, "/data", None).unwrap());
        assert_ne!(generated, resolve(&another, "/", None).unwrap());
        assert_eq!(
            fs::metadata(base.join("data").join(FILE_NAME))
                .unwrap()
                .permissions()
                .mode()
                & 0o777,
            0o600
        );
        fs::remove_dir_all(base).unwrap();
        fs::remove_dir_all(another).unwrap();
    }

    #[test]
    fn concurrent_initialization_keeps_a_single_identity() {
        let base = directory();
        let handles: Vec<_> = (0..8)
            .map(|_| {
                let path = base.clone();
                std::thread::spawn(move || resolve(&path, "/", None).unwrap())
            })
            .collect();
        let values: Vec<_> = handles
            .into_iter()
            .map(|thread| thread.join().unwrap())
            .collect();
        assert!(values.iter().all(|value| *value == values[0]));
        fs::remove_dir_all(base).unwrap();
    }

    #[test]
    fn legacy_identity_requires_override() {
        let base = directory();
        fs::write(base.join(LEGACY_FILE_NAME), "hash").unwrap();
        assert_eq!(
            resolve(&base, "/", None).unwrap_err().code,
            "WRAPPER_DEVICE_INFO_LEGACY"
        );
        assert_eq!(
            resolve(&base, "/", Some("a/b/c/d/e/f/g/h/i")).unwrap(),
            "a/b/c/d/e/f/g/h/i"
        );
        fs::remove_dir_all(base).unwrap();
    }

    #[test]
    fn refuses_device_mismatch_and_invalid_stored_identity() {
        let base = directory();
        let identity = resolve(&base, "/", None).unwrap();
        assert_eq!(
            resolve(&base, "/", Some("a/b/c/d/e/f/g/h/i"))
                .unwrap_err()
                .code,
            "WRAPPER_DEVICE_INFO_MISMATCH"
        );
        assert_eq!(resolve(&base, "/", Some(&identity)).unwrap(), identity);
        fs::write(base.join(FILE_NAME), "invalid\n").unwrap();
        assert_eq!(
            resolve(&base, "/", None).unwrap_err().code,
            "WRAPPER_DEVICE_INFO"
        );
        fs::remove_dir_all(base).unwrap();
    }
}

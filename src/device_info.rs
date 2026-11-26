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
// Match the known-working Apple Music client fingerprint used by the
// upstream Wrapper implementation. Android IDs in this field are 64-bit
// values rendered as 16 lowercase hexadecimal characters.
const DEVICE_PREFIX: &str = "Music/5.0.2/Android/10/Pixel 8/7663314/en-US/en-US/";

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

fn random_hex<const N: usize>() -> Result<String, ApiError> {
    let mut random = [0u8; N];
    File::open("/dev/urandom")
        .and_then(|mut file| file.read_exact(&mut random))
        .map_err(|_| invalid())?;
    let mut suffix = String::with_capacity(N * 2);
    for byte in random {
        write!(suffix, "{byte:02x}").map_err(|_| invalid())?;
    }
    Ok(suffix)
}

fn random_android_id() -> Result<String, ApiError> {
    // The native Android client fingerprint uses an eight-byte / 64-bit ID.
    random_hex::<8>()
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

    let identity = format!("{DEVICE_PREFIX}{}", random_android_id()?);
    // Keep the temporary filename's entropy at 128 bits; it is independent
    // of the 64-bit Android ID format required by the client fingerprint.
    let temp_path = directory.join(format!(
        ".wrapper-device-info-{}.tmp",
        random_hex::<16>()?
    ));
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
            std::env::temp_dir().join(format!("wrapper-identity-{}", random_hex::<8>().unwrap()));
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
    fn fresh_identity_uses_known_client_prefix_and_64_bit_android_id() {
        let base = directory();
        let generated = resolve(&base, "/", None).unwrap();
        let android_id = generated
            .strip_prefix(DEVICE_PREFIX)
            .expect("generated identity must use the pinned client prefix");

        assert_eq!(android_id.len(), 16);
        assert!(android_id
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte)));

        fs::remove_dir_all(base).unwrap();
    }

    #[test]
    fn existing_generated_identity_is_never_migrated_or_rotated() {
        let base = directory();
        // Existing 1.0.0 installations may have the previous 32-hex suffix.
        let legacy = "Music/4.9/Android/10/Samsung S9/7663313/en-US/en-US/00112233445566778899aabbccddeeff";
        fs::write(base.join(FILE_NAME), format!("{legacy}\n")).unwrap();

        assert_eq!(resolve(&base, "/", None).unwrap(), legacy);
        assert_eq!(
            fs::read_to_string(base.join(FILE_NAME)).unwrap(),
            format!("{legacy}\n")
        );

        fs::remove_dir_all(base).unwrap();
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

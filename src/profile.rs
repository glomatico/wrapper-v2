//! Runtime identity comes from the same pinned JSON profiles used by staging/CMake.
use crate::error::ApiError;
use base64::{engine::general_purpose::STANDARD, Engine as _};
use serde::Deserialize;
use serde_json::Value;
use zeroize::Zeroizing;

#[derive(Clone, Debug, Deserialize)]
pub struct ContextContract {
    ctx_bytes: usize,
    state_bytes: usize,
    byte_fields: Vec<String>,
}
use std::{fs::File, io::Read, path::Path};

#[derive(Clone, Debug, Deserialize)]
pub struct RuntimeProfile {
    pub architecture: String,
    pub linux_architecture: String,
    pub profile: String,
    pub context_profile: Option<String>,
    pub context_supported: bool,
    pub context_contract: Option<ContextContract>,
    pub elf_machine: u16,
}
impl RuntimeProfile {
    pub fn validate_context(&self, context: &Value) -> bool {
        let Some(contract) = &self.context_contract else {
            return false;
        };
        self.context_supported
            && context.get("profile").and_then(Value::as_str) == self.context_profile.as_deref()
            && [("ctx", contract.ctx_bytes), ("state", contract.state_bytes)]
                .iter()
                .all(|(key, size)| {
                    context.get(key).and_then(Value::as_str).is_some_and(|s| {
                        STANDARD
                            .decode(s)
                            .map(Zeroizing::new)
                            .is_ok_and(|data| data.len() == *size)
                    })
                })
            && contract.byte_fields.iter().all(|key| {
                context
                    .get(key)
                    .and_then(Value::as_u64)
                    .is_some_and(|n| n <= 255)
            })
    }
    pub fn select(arch: &str) -> Result<Self, ApiError> {
        let source = match arch {
            "x86_64" | "amd64" => include_str!("../profiles/apple-music-1109-x86_64.json"),
            "arm64-v8a" | "aarch64" | "arm64" => {
                include_str!("../profiles/apple-music-1109-arm64-v8a.json")
            }
            _ => return Err(ApiError::bad_config("WRAPPER_ARCH")),
        };
        serde_json::from_str(source).map_err(|_| ApiError::bad_config("runtime_profile"))
    }
    pub fn validate_rootfs(&self, rootfs: &Path) -> Result<(), ApiError> {
        let mut header = [0u8; 20];
        File::open(rootfs.join("system/bin/apple-music-worker"))
            .and_then(|mut f| f.read_exact(&mut header))
            .map_err(|_| ApiError::bad_config("rootfs_worker"))?;
        if &header[..6] != b"\x7fELF\x02\x01"
            || u16::from_le_bytes([header[18], header[19]]) != self.elf_machine
        {
            return Err(ApiError::bad_config("rootfs_architecture"));
        }
        // Old verified x86 rootfs layouts did not include metadata. The live
        // worker handshake remains mandatory in both layouts.
        let path = rootfs.join("worker-profile.json");
        if path.exists() {
            let metadata: serde_json::Value = serde_json::from_reader(
                File::open(path).map_err(|_| ApiError::bad_config("rootfs_profile"))?,
            )
            .map_err(|_| ApiError::bad_config("rootfs_profile"))?;
            if metadata["architecture"].as_str() != Some(self.architecture.as_str())
                || metadata["profile"].as_str() != Some(self.profile.as_str())
                || metadata["context_supported"].as_bool() != Some(self.context_supported)
                || metadata["context_profile"].as_str() != self.context_profile.as_deref()
            {
                return Err(ApiError::bad_config("rootfs_profile"));
            }
        }
        Ok(())
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    #[test]
    fn aliases_select_distinct_verified_profiles() {
        let arm = RuntimeProfile::select("aarch64").unwrap();
        assert_eq!(arm.architecture, "arm64-v8a");
        assert_eq!(arm.elf_machine, 183);
        let x86 = RuntimeProfile::select("amd64").unwrap();
        assert_eq!(x86.elf_machine, 62);
        assert_ne!(arm.profile, x86.profile);
        assert!(RuntimeProfile::select("riscv64").is_err());
    }
    #[test]
    fn context_contract_rejects_malformed_and_cross_architecture_data() {
        let x86 = RuntimeProfile::select("x86_64").unwrap();
        let arm64 = RuntimeProfile::select("arm64-v8a").unwrap();
        let valid = json!({
            "profile":x86.context_profile,
            "ctx":STANDARD.encode(vec![0; 0x8000]),
            "state":STANDARD.encode(vec![0; 0x800]),
            "bl":0,
            "r9b":255,
        });
        assert!(x86.validate_context(&valid));
        assert!(!arm64.validate_context(&valid));
        for (field, bad) in [
            ("profile", json!("wrong")),
            ("ctx", json!("")),
            ("state", json!("invalid-base64!")),
            ("bl", json!(256)),
            ("r9b", json!(-1)),
        ] {
            let mut changed = valid.clone();
            changed[field] = bad;
            assert!(!x86.validate_context(&changed), "field: {field}");
        }
    }
}

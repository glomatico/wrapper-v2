use std::io::{self, Read, Write};

pub const MAGIC: u32 = 0x57563249; // WV2I
pub const VERSION: u16 = 1;
pub const KIND_REQUEST: u16 = 1;
pub const KIND_RESPONSE: u16 = 2;

pub const OP_HEALTH: u16 = 1;
pub const OP_ME: u16 = 2;
pub const OP_LOGIN: u16 = 3;
pub const OP_LOGIN_2FA: u16 = 4;
pub const OP_LOGOUT: u16 = 5;
pub const OP_PLAYBACK: u16 = 6;
pub const OP_DECRYPT_BATCH: u16 = 7;

pub const DECRYPT_MAGIC: u32 = 0x57563244; // WV2D
pub const DECRYPT_VERSION: u16 = 1;
pub const DECRYPT_KIND_BATCH: u16 = 1;
pub const DECRYPT_KIND_OK: u16 = 2;
pub const DECRYPT_KIND_ERROR: u16 = 3;
pub const DECRYPT_KIND_CLOSE: u16 = 9;

const HEADER_LEN: usize = 20;
const DECRYPT_HEADER_LEN: usize = 16;

#[derive(Debug, Clone)]
pub struct Frame {
    pub kind: u16,
    pub request_id: u32,
    pub opcode: u16,
    pub flags: u16,
    pub payload: Vec<u8>,
}

#[derive(Debug, Clone)]
pub struct DecryptFrame {
    pub kind: u16,
    pub request_id: u32,
    pub payload: Vec<u8>,
}

pub fn write_frame(mut w: impl Write, frame: &Frame) -> io::Result<()> {
    if frame.payload.len() > u32::MAX as usize {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "payload too large",
        ));
    }
    w.write_all(&MAGIC.to_be_bytes())?;
    w.write_all(&VERSION.to_be_bytes())?;
    w.write_all(&frame.kind.to_be_bytes())?;
    w.write_all(&frame.request_id.to_be_bytes())?;
    w.write_all(&frame.opcode.to_be_bytes())?;
    w.write_all(&frame.flags.to_be_bytes())?;
    w.write_all(&(frame.payload.len() as u32).to_be_bytes())?;
    w.write_all(&frame.payload)?;
    w.flush()
}

pub fn read_decrypt_frame(mut r: impl Read) -> io::Result<DecryptFrame> {
    let mut h = [0u8; DECRYPT_HEADER_LEN];
    r.read_exact(&mut h)?;
    let magic = u32::from_be_bytes([h[0], h[1], h[2], h[3]]);
    let version = u16::from_be_bytes([h[4], h[5]]);
    if magic != DECRYPT_MAGIC {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "bad decrypt magic",
        ));
    }
    if version != DECRYPT_VERSION {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "bad decrypt version",
        ));
    }
    let kind = u16::from_be_bytes([h[6], h[7]]);
    let request_id = u32::from_be_bytes([h[8], h[9], h[10], h[11]]);
    let payload_len = u32::from_be_bytes([h[12], h[13], h[14], h[15]]) as usize;
    let mut payload = vec![0u8; payload_len];
    r.read_exact(&mut payload)?;
    Ok(DecryptFrame {
        kind,
        request_id,
        payload,
    })
}

pub fn write_decrypt_frame(mut w: impl Write, frame: &DecryptFrame) -> io::Result<()> {
    if frame.payload.len() > u32::MAX as usize {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "decrypt payload too large",
        ));
    }
    w.write_all(&DECRYPT_MAGIC.to_be_bytes())?;
    w.write_all(&DECRYPT_VERSION.to_be_bytes())?;
    w.write_all(&frame.kind.to_be_bytes())?;
    w.write_all(&frame.request_id.to_be_bytes())?;
    w.write_all(&(frame.payload.len() as u32).to_be_bytes())?;
    w.write_all(&frame.payload)?;
    w.flush()
}

pub fn read_frame(mut r: impl Read) -> io::Result<Frame> {
    let mut h = [0u8; HEADER_LEN];
    r.read_exact(&mut h)?;
    let magic = u32::from_be_bytes([h[0], h[1], h[2], h[3]]);
    let version = u16::from_be_bytes([h[4], h[5]]);
    if magic != MAGIC {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "bad ipc magic"));
    }
    if version != VERSION {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "bad ipc version",
        ));
    }
    let kind = u16::from_be_bytes([h[6], h[7]]);
    let request_id = u32::from_be_bytes([h[8], h[9], h[10], h[11]]);
    let opcode = u16::from_be_bytes([h[12], h[13]]);
    let flags = u16::from_be_bytes([h[14], h[15]]);
    let payload_len = u32::from_be_bytes([h[16], h[17], h[18], h[19]]) as usize;
    let mut payload = vec![0u8; payload_len];
    r.read_exact(&mut payload)?;
    Ok(Frame {
        kind,
        request_id,
        opcode,
        flags,
        payload,
    })
}

pub fn decrypt_batch_payload(adam: &str, uri: &str, samples: &[Vec<u8>]) -> io::Result<Vec<u8>> {
    if adam.len() > u16::MAX as usize
        || uri.len() > u16::MAX as usize
        || samples.len() > u32::MAX as usize
    {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "decrypt payload too large",
        ));
    }
    if samples.is_empty() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "decrypt batch must not be empty",
        ));
    }
    let mut size = 8usize
        .checked_add(samples.len().checked_mul(4).ok_or_else(|| {
            io::Error::new(io::ErrorKind::InvalidInput, "decrypt payload too large")
        })?)
        .and_then(|n| n.checked_add(adam.len()))
        .and_then(|n| n.checked_add(uri.len()))
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "decrypt payload too large"))?;
    for sample in samples {
        if sample.is_empty() || sample.len() > u32::MAX as usize {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "invalid decrypt sample length",
            ));
        }
        size = size.checked_add(sample.len()).ok_or_else(|| {
            io::Error::new(io::ErrorKind::InvalidInput, "decrypt payload too large")
        })?;
    }
    let mut out = Vec::with_capacity(size);
    out.extend_from_slice(&(adam.len() as u16).to_be_bytes());
    out.extend_from_slice(&(uri.len() as u16).to_be_bytes());
    out.extend_from_slice(&(samples.len() as u32).to_be_bytes());
    for sample in samples {
        out.extend_from_slice(&(sample.len() as u32).to_be_bytes());
    }
    out.extend_from_slice(adam.as_bytes());
    out.extend_from_slice(uri.as_bytes());
    for sample in samples {
        out.extend_from_slice(sample);
    }
    Ok(out)
}

pub fn parse_decrypt_samples_payload(data: &[u8]) -> io::Result<Vec<Vec<u8>>> {
    if data.len() < 4 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "decrypt response too short",
        ));
    }
    let sample_count = u32::from_be_bytes([data[0], data[1], data[2], data[3]]) as usize;
    let table_end = 4usize
        .checked_add(sample_count.checked_mul(4).ok_or_else(|| {
            io::Error::new(io::ErrorKind::InvalidData, "decrypt response too large")
        })?)
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "decrypt response too large"))?;
    if data.len() < table_end {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "truncated decrypt length table",
        ));
    }
    let mut lengths = Vec::with_capacity(sample_count);
    for i in 0..sample_count {
        let off = 4 + i * 4;
        lengths.push(
            u32::from_be_bytes([data[off], data[off + 1], data[off + 2], data[off + 3]]) as usize,
        );
    }
    let mut offset = table_end;
    let mut out = Vec::with_capacity(sample_count);
    for len in lengths {
        let end = offset.checked_add(len).ok_or_else(|| {
            io::Error::new(io::ErrorKind::InvalidData, "decrypt response too large")
        })?;
        if end > data.len() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "truncated decrypt sample",
            ));
        }
        out.push(data[offset..end].to_vec());
        offset = end;
    }
    if offset != data.len() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "trailing decrypt response bytes",
        ));
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn frame_round_trip() {
        let frame = Frame {
            kind: KIND_REQUEST,
            request_id: 42,
            opcode: OP_HEALTH,
            flags: 7,
            payload: b"hello".to_vec(),
        };
        let mut bytes = Vec::new();
        write_frame(&mut bytes, &frame).unwrap();
        let decoded = read_frame(bytes.as_slice()).unwrap();
        assert_eq!(decoded.kind, frame.kind);
        assert_eq!(decoded.request_id, frame.request_id);
        assert_eq!(decoded.opcode, frame.opcode);
        assert_eq!(decoded.flags, frame.flags);
        assert_eq!(decoded.payload, frame.payload);
    }

    #[test]
    fn bad_magic_is_rejected() {
        let mut bytes = vec![0u8; 20];
        bytes[4..6].copy_from_slice(&VERSION.to_be_bytes());
        let err = read_frame(bytes.as_slice()).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidData);
    }

    #[test]
    fn decrypt_batch_payload_layout() {
        let out = decrypt_batch_payload("123", "skd://x", &[vec![1, 2, 3, 4]]).unwrap();
        assert_eq!(&out[0..2], &3u16.to_be_bytes());
        assert_eq!(&out[2..4], &7u16.to_be_bytes());
        assert_eq!(&out[4..8], &1u32.to_be_bytes());
        assert_eq!(&out[8..12], &4u32.to_be_bytes());
        assert_eq!(&out[12..15], b"123");
        assert_eq!(&out[15..22], b"skd://x");
        assert_eq!(&out[22..], &[1, 2, 3, 4]);
    }

    #[test]
    fn decrypt_samples_payload_round_trip() {
        let data = [
            2u32.to_be_bytes().as_slice(),
            3u32.to_be_bytes().as_slice(),
            2u32.to_be_bytes().as_slice(),
            b"abc",
            b"de",
        ]
        .concat();
        assert_eq!(
            parse_decrypt_samples_payload(&data).unwrap(),
            vec![b"abc".to_vec(), b"de".to_vec()]
        );
    }
}

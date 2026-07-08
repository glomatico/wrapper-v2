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
pub const OP_DECRYPT_SAMPLE: u16 = 7;

const HEADER_LEN: usize = 20;

#[derive(Debug, Clone)]
pub struct Frame {
    pub kind: u16,
    pub request_id: u32,
    pub opcode: u16,
    pub flags: u16,
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

pub fn decrypt_payload(adam: &str, uri: &str, sample: &[u8]) -> io::Result<Vec<u8>> {
    if adam.len() > u16::MAX as usize
        || uri.len() > u16::MAX as usize
        || sample.len() > u32::MAX as usize
    {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "decrypt payload too large",
        ));
    }
    let mut out = Vec::with_capacity(8 + adam.len() + uri.len() + sample.len());
    out.extend_from_slice(&(adam.len() as u16).to_be_bytes());
    out.extend_from_slice(&(uri.len() as u16).to_be_bytes());
    out.extend_from_slice(&(sample.len() as u32).to_be_bytes());
    out.extend_from_slice(adam.as_bytes());
    out.extend_from_slice(uri.as_bytes());
    out.extend_from_slice(sample);
    Ok(out)
}

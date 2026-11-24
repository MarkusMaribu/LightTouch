//! A6-family factory PPN media and the A6DI stream used by the native restore helper.
use crate::error::Result;
use crate::posio;
use std::fs::{self, File};
use std::io::{BufWriter, Write};
use std::path::Path;

pub const PAGE_BYTES: u32 = 16_384;
pub const SPARE_BYTES: u32 = 64;
pub const PAGES_PER_BLOCK: u32 = 256;
pub const BLOCKS_PER_CHIP: u32 = 2128;
const SECTOR_BYTES: usize = 4096;
const RECORD_BYTES: usize = 65_536;
const MEDIA_BYTES: u64 = 16_000_000_000;
const SYSTEM_START: u64 = 16 << 20;

fn put(dst: &mut [u8], at: usize, value: u32, bytes: usize) {
    dst[at..at + bytes].copy_from_slice(&value.to_le_bytes()[..bytes]);
}

fn checksum(dst: &mut [u8], at: usize, bytes: usize) {
    let mut a = 255u32;
    let mut c = 255u32;
    for chunk in dst[at..at + bytes].chunks(21) {
        for &byte in chunk {
            a += u32::from(byte);
            c += a;
        }
        a = (a & 255) + (a >> 8);
        c = (c & 255) + (c >> 8);
    }
    for _ in 0..2 {
        a = (a & 255) + (a >> 8);
        c = (c & 255) + (c >> 8);
    }
    dst[at + bytes..at + bytes + 2].copy_from_slice(&[a as u8, c as u8]);
}

/// Factory partitions and boot-unit reservations for the virtual chip, from upstream FirmwareKit's A6NAND.
fn factory_header() -> [u8; 1536] {
    let mut header = [0; 1536];
    header[..4].copy_from_slice(b"ndrG");
    put(&mut header, 8, 1, 4);
    put(&mut header, 12, 9, 4);
    let geometry = [1, 2, BLOCKS_PER_CHIP, PAGES_PER_BLOCK, PAGE_BYTES / 512, SPARE_BYTES, 6, 0xdead_cafe];
    for (index, value) in geometry.into_iter().enumerate() {
        put(&mut header, 0x200 + 4 * index, value, 4);
    }
    put(&mut header, 0x224, 0xdead_cafe, 4);
    checksum(&mut header, 0x224, 42);

    let partitions = [
        (b"toob", 0, 1, 2),
        (b"golp", 4, 1, 0x108),
        (b"mrvn", 2, 3, 0x108),
        (b"mrif", 2, 1, 0x308),
        (b"sysf", 1, BLOCKS_PER_CHIP - 1, 6),
        (b"gfcs", 0, 1, 1),
        (b"gaid", 0, 1, 1),
        (b"tbbf", 0, 1, 1),
    ];
    for index in 0..32 {
        let (name, start, count, flags) = partitions.get(index).copied().unwrap_or((b"enon", 0, 0, 0));
        let at = 0x400 + 16 * index;
        header[at..at + 4].copy_from_slice(name);
        put(&mut header, at + 4, start, 4);
        put(&mut header, at + 8, count, 4);
        put(&mut header, at + 12, flags, 4);
    }

    header[0x250..0x254].copy_from_slice(b"rAps");
    put(&mut header, 0x254, 12, 2);
    header[0x35c..0x360].copy_from_slice(b"sbus");
    put(&mut header, 0x360, 12, 2);
    put(&mut header, 0x362, 2, 1);
    for index in 0..12 {
        put(&mut header, 0x25a + 4 * index, 1 + index as u32 / 2, 3);
        put(&mut header, 0x25d + 4 * index, index as u32 % 2, 1);
        put(&mut header, 0x366 + index, 2 + index as u32, 1);
    }
    checksum(&mut header, 0x250, 0x10a);
    checksum(&mut header, 0x35c, 0x4a);
    header
}

/// Create an exclusive factory store. Unwritten pages read as erased to the guest.
pub fn create_factory(out: &Path) -> Result<()> {
    fs::create_dir(out)?;
    let output_files = ["geometry.json", "bus0-ce0.pages", "bus1-ce0.pages"];
    let result = (|| -> Result<()> {
        let meta = serde_json::json!({
            "page_bytes": PAGE_BYTES, "spare_bytes": SPARE_BYTES, "pages_per_block": PAGES_PER_BLOCK,
            "blocks_per_ce": BLOCKS_PER_CHIP, "ce_per_bus": 1, "buses": 2, "chip_id": "0x014E5050",
            "storage_format": "nand-xor-ff-v2", "layout": "ppn-factory",
        });
        fs::write(out.join(output_files[0]), meta.to_string())?;
        let stride = (PAGE_BYTES + SPARE_BYTES) as usize;
        let mut record = vec![0; stride];
        for (physical, factory) in record.iter_mut().zip(factory_header()) {
            *physical = factory ^ 0xff;
        }
        let chip_bytes = u64::from(BLOCKS_PER_CHIP) * u64::from(PAGES_PER_BLOCK) * stride as u64;
        for name in &output_files[1..] {
            let mut chip = posio::create_sparse(&out.join(name))?;
            chip.set_len(chip_bytes)?;
            chip.write_all(&record)?;
            chip.write_all(&record)?;
            chip.sync_all()?;
        }
        Ok(())
    })();
    if result.is_err() {
        for name in output_files {
            let _ = fs::remove_file(out.join(name));
        }
        let _ = fs::remove_dir(out);
    }
    result
}

fn write_record(out: &mut impl Write, offset: u64, payload: &[u8]) -> std::io::Result<()> {
    let mut crc = flate2::Crc::new();
    crc.update(payload);
    out.write_all(&offset.to_le_bytes())?;
    out.write_all(&(payload.len() as u32).to_le_bytes())?;
    out.write_all(&crc.sum().to_le_bytes())?;
    out.write_all(payload)
}

/// Stream the System volume and the two-partition MBR. The restore helper formats Data in the guest.
pub fn write_restore_stream(system: &Path, out: &Path) -> Result<()> {
    let image = File::open(system)?;
    let system_bytes = image.metadata()?.len();
    let mut hfs = [0; 512];
    posio::read_exact_at(&image, &mut hfs, 1024)?;
    let block_bytes = u32::from_be_bytes(hfs[40..44].try_into().unwrap());
    let blocks = u32::from_be_bytes(hfs[44..48].try_into().unwrap());
    if !matches!(&hfs[..2], b"H+" | b"HX")
        || block_bytes as usize != SECTOR_BYTES
        || u64::from(block_bytes) * u64::from(blocks) != system_bytes
        || system_bytes >= MEDIA_BYTES - SYSTEM_START
    {
        return Err(crate::unsupported!("System must be a complete 4096-byte-block HFS+ volume that fits on A6 media"));
    }
    let sector = SECTOR_BYTES as u64;
    let data_start = SYSTEM_START + system_bytes;
    let partitions = [(SYSTEM_START, system_bytes), (data_start, MEDIA_BYTES - data_start)];
    let mut mbr = [0; SECTOR_BYTES];
    for (index, (start, length)) in partitions.into_iter().enumerate() {
        let at = 446 + 16 * index;
        mbr[at..at + 8].copy_from_slice(&[0, 254, 255, 255, 0xaf, 254, 255, 255]);
        put(&mut mbr, at + 8, (start / sector) as u32, 4);
        put(&mut mbr, at + 12, (length / sector) as u32, 4);
    }
    mbr[510..512].copy_from_slice(b"\x55\xaa");

    let output = File::options().write(true).create_new(true).open(out)?;
    let result = (|| -> Result<()> {
        let mut stream = BufWriter::new(output);
        let records = 1 + system_bytes.div_ceil(RECORD_BYTES as u64) as u32;
        stream.write_all(b"A6DI")?;
        stream.write_all(&1u32.to_le_bytes())?;
        stream.write_all(&MEDIA_BYTES.to_le_bytes())?;
        stream.write_all(&records.to_le_bytes())?;
        stream.write_all(&0u32.to_le_bytes())?;
        write_record(&mut stream, 0, &mbr)?;
        let mut offset = 0;
        let mut buffer = vec![0; RECORD_BYTES];
        while offset < system_bytes {
            let bytes = (system_bytes - offset).min(RECORD_BYTES as u64) as usize;
            posio::read_exact_at(&image, &mut buffer[..bytes], offset)?;
            write_record(&mut stream, SYSTEM_START + offset, &buffer[..bytes])?;
            offset += bytes as u64;
        }
        stream.flush()?;
        stream.get_ref().sync_all()?;
        Ok(())
    })();
    if result.is_err() {
        let _ = fs::remove_file(out);
    }
    result
}

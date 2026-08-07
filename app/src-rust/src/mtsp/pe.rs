use std::path::Path;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum D3dApi {
    D3D9,
    D3D10,
    D3D11,
    D3D12,
    Unknown,
}

#[derive(Debug)]
pub struct PeInfo {
    pub machine_type: u16,
    pub is_64_bit: bool,
    pub imports: Vec<String>,
    pub detected_api: D3dApi,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AgilityExports {
    pub sdk_version: u32,
    pub sdk_path: String,
}

const MACHINE_I386: u16 = 0x014c;
const MACHINE_AMD64: u16 = 0x8664;

/// True when the PE image carries a COM descriptor data directory — the
/// signature of a managed (.NET / CIL) assembly. Native executables (Unity
/// player exes, plain PE binaries) have no COM descriptor and CANNOT be
/// executed by the Mono runtime ("File does not contain a valid CIL image").
pub fn is_dotnet_assembly(data: &[u8]) -> bool {
    let Some(view) = PeView::parse(data) else {
        return false;
    };
    // Data directories start after the optional header's fixed fields:
    //   PE32  (magic 0x10b): optional + 0x60
    //   PE32+ (magic 0x20b): optional + 0x70
    // The COM descriptor is directory index 14 (14 * 8 bytes per entry).
    let optional = view.pe_offset + 24;
    let dirs_start = match view.optional_magic {
        0x10b => optional + 0x60,
        0x20b => optional + 0x70,
        _ => return false,
    };
    let com_entry = dirs_start + 14 * 8;
    if com_entry + 4 > data.len() {
        return false;
    }
    let rva = u32::from_le_bytes([data[com_entry], data[com_entry + 1], data[com_entry + 2], data[com_entry + 3]]);
    rva != 0
}

pub fn parse_pe_imports(data: &[u8]) -> Option<PeInfo> {
    let view = PeView::parse(data)?;
    let machine = view.machine;
    let is_64_bit = machine == MACHINE_AMD64;
    let optional_magic = view.optional_magic;

    let (import_dir_rva, import_dir_size) = if optional_magic == 0x20b {
        if view.pe_offset + 24 + 112 + 8 > data.len() {
            return Some(PeInfo { machine_type: machine, is_64_bit, imports: vec![], detected_api: D3dApi::Unknown });
        }
        let rva = u32::from_le_bytes(data[view.pe_offset + 24 + 112..view.pe_offset + 24 + 116].try_into().ok()?);
        let size = u32::from_le_bytes(data[view.pe_offset + 24 + 116..view.pe_offset + 24 + 120].try_into().ok()?);
        (rva, size)
    } else if optional_magic == 0x10b {
        if view.pe_offset + 24 + 96 + 8 > data.len() {
            return Some(PeInfo { machine_type: machine, is_64_bit, imports: vec![], detected_api: D3dApi::Unknown });
        }
        let rva = u32::from_le_bytes(data[view.pe_offset + 24 + 96..view.pe_offset + 24 + 100].try_into().ok()?);
        let size = u32::from_le_bytes(data[view.pe_offset + 24 + 100..view.pe_offset + 24 + 104].try_into().ok()?);
        (rva, size)
    } else {
        return Some(PeInfo { machine_type: machine, is_64_bit, imports: vec![], detected_api: D3dApi::Unknown });
    };

    if import_dir_rva == 0 || import_dir_size == 0 {
        return Some(PeInfo { machine_type: machine, is_64_bit, imports: vec![], detected_api: D3dApi::Unknown });
    }

    let mut imports = Vec::new();
    for section in &view.sections {
        let sec_end = section.virtual_address + section.virtual_size;

        if import_dir_rva >= section.virtual_address && import_dir_rva < sec_end {
            let dir_file_offset = section.raw_data_ptr + (import_dir_rva - section.virtual_address) as usize;

            let mut entry_off = dir_file_offset;
            loop {
                if entry_off + 20 > data.len() {
                    break;
                }
                let ilt_rva = u32::from_le_bytes(data[entry_off..entry_off + 4].try_into().ok()?);
                let name_rva = u32::from_le_bytes(data[entry_off + 12..entry_off + 16].try_into().ok()?);
                if ilt_rva == 0 && name_rva == 0 {
                    break;
                }
                if name_rva >= section.virtual_address && name_rva < sec_end {
                    let name_off = section.raw_data_ptr + (name_rva - section.virtual_address) as usize;
                    if let Some(dll_name) = read_null_terminated(data, name_off) {
                        imports.push(dll_name);
                    }
                }
                entry_off += 20;
                if imports.len() > 256 {
                    break;
                }
            }
            break;
        }
    }

    let detected_api = detect_d3d_api(&imports);

    Some(PeInfo { machine_type: machine, is_64_bit, imports, detected_api })
}

#[derive(Debug, Clone, Copy)]
struct PeSection {
    virtual_size: u32,
    virtual_address: u32,
    raw_data_size: u32,
    raw_data_ptr: usize,
}

#[derive(Debug)]
struct PeView {
    pe_offset: usize,
    machine: u16,
    optional_magic: u16,
    sections: Vec<PeSection>,
}

impl PeView {
    fn parse(data: &[u8]) -> Option<Self> {
        if data.len() < 64 {
            return None;
        }
        if data[0] != b'M' || data[1] != b'Z' {
            return None;
        }

        let pe_offset = u32::from_le_bytes(data[0x3c..0x40].try_into().ok()?) as usize;
        if pe_offset + 24 > data.len() {
            return None;
        }
        if data[pe_offset] != b'P'
            || data[pe_offset + 1] != b'E'
            || data[pe_offset + 2] != 0
            || data[pe_offset + 3] != 0
        {
            return None;
        }

        let machine = u16::from_le_bytes(data[pe_offset + 4..pe_offset + 6].try_into().ok()?);
        let num_sections = u16::from_le_bytes(data[pe_offset + 6..pe_offset + 8].try_into().ok()?) as usize;
        let optional_hdr_size = u16::from_le_bytes(data[pe_offset + 20..pe_offset + 22].try_into().ok()?) as usize;
        let optional_magic = u16::from_le_bytes(data[pe_offset + 24..pe_offset + 26].try_into().ok()?);
        let section_offset = pe_offset + 24 + optional_hdr_size;

        let mut sections = Vec::with_capacity(num_sections);
        for i in 0..num_sections {
            let sec_off = section_offset + i * 40;
            if sec_off + 40 > data.len() {
                break;
            }
            sections.push(PeSection {
                virtual_size: u32::from_le_bytes(data[sec_off + 8..sec_off + 12].try_into().ok()?),
                virtual_address: u32::from_le_bytes(data[sec_off + 12..sec_off + 16].try_into().ok()?),
                raw_data_size: u32::from_le_bytes(data[sec_off + 16..sec_off + 20].try_into().ok()?),
                raw_data_ptr: u32::from_le_bytes(data[sec_off + 20..sec_off + 24].try_into().ok()?) as usize,
            });
        }

        Some(Self { pe_offset, machine, optional_magic, sections })
    }

    fn data_dir_rva(&self, data: &[u8], index: usize) -> Option<u32> {
        let base = self.pe_offset + 24 + if self.optional_magic == 0x20b { 112 } else { 96 };
        let entry = base + index * 8;
        if entry + 8 > data.len() {
            return None;
        }
        Some(u32::from_le_bytes(data[entry..entry + 4].try_into().ok()?))
    }

    fn rva_to_file_offset(&self, rva: u32) -> Option<usize> {
        for section in &self.sections {
            let span = section.virtual_size.max(section.raw_data_size);
            if rva >= section.virtual_address && rva < section.virtual_address + span {
                return Some(section.raw_data_ptr + (rva - section.virtual_address) as usize);
            }
        }
        None
    }
}

pub fn parse_agility_exports(data: &[u8]) -> Option<AgilityExports> {
    Some(AgilityExports {
        sdk_version: parse_export_u32(data, "D3D12SDKVersion")?,
        sdk_path: parse_export_string(data, "D3D12SDKPath")?,
    })
}

pub fn parse_export_u32(data: &[u8], symbol: &str) -> Option<u32> {
    let export_offset = export_symbol_offset(data, symbol)?;
    let bytes = data.get(export_offset..export_offset + 4)?;
    Some(u32::from_le_bytes(bytes.try_into().ok()?))
}

pub fn parse_export_string(data: &[u8], symbol: &str) -> Option<String> {
    let export_offset = export_symbol_offset(data, symbol)?;
    read_null_terminated(data, export_offset)
}

fn export_symbol_offset(data: &[u8], symbol: &str) -> Option<usize> {
    let view = PeView::parse(data)?;
    let export_rva = view.data_dir_rva(data, 0)?;
    if export_rva == 0 {
        return None;
    }
    let export_off = view.rva_to_file_offset(export_rva)?;
    let number_of_names = u32::from_le_bytes(data.get(export_off + 24..export_off + 28)?.try_into().ok()?);
    let address_of_functions = u32::from_le_bytes(data.get(export_off + 28..export_off + 32)?.try_into().ok()?);
    let address_of_names = u32::from_le_bytes(data.get(export_off + 32..export_off + 36)?.try_into().ok()?);
    let address_of_name_ordinals = u32::from_le_bytes(data.get(export_off + 36..export_off + 40)?.try_into().ok()?);

    let functions_off = view.rva_to_file_offset(address_of_functions)?;
    let names_off = view.rva_to_file_offset(address_of_names)?;
    let ordinals_off = view.rva_to_file_offset(address_of_name_ordinals)?;

    for index in 0..number_of_names as usize {
        let name_rva = u32::from_le_bytes(data.get(names_off + index * 4..names_off + index * 4 + 4)?.try_into().ok()?);
        let name_off = view.rva_to_file_offset(name_rva)?;
        if read_null_terminated(data, name_off)?.as_str() != symbol {
            continue;
        }

        let ordinal =
            u16::from_le_bytes(data.get(ordinals_off + index * 2..ordinals_off + index * 2 + 2)?.try_into().ok()?);
        let function_rva = u32::from_le_bytes(
            data.get(functions_off + ordinal as usize * 4..functions_off + ordinal as usize * 4 + 4)?
                .try_into()
                .ok()?,
        );
        return view.rva_to_file_offset(function_rva);
    }

    None
}

fn read_null_terminated(data: &[u8], offset: usize) -> Option<String> {
    let end = data.iter().skip(offset).position(|&b| b == 0)?;
    Some(String::from_utf8_lossy(&data[offset..offset + end]).to_string())
}

pub fn detect_d3d_api(imports: &[String]) -> D3dApi {
    let lower: Vec<String> = imports.iter().map(|s| s.to_lowercase()).collect();

    if lower.iter().any(|d| d == "d3d12.dll") {
        return D3dApi::D3D12;
    }
    if lower.iter().any(|d| d == "d3d11.dll") {
        return D3dApi::D3D11;
    }
    if lower.iter().any(|d| d == "d3d10.dll" || d == "d3d10_1.dll" || d == "d3d10core.dll") {
        return D3dApi::D3D10;
    }
    if lower.iter().any(|d| d == "d3d9.dll") {
        return D3dApi::D3D9;
    }

    D3dApi::Unknown
}

pub fn analyze_game_exe(game_dir: &Path) -> Option<PeInfo> {
    if let Ok(entries) = std::fs::read_dir(game_dir) {
        for entry in entries.flatten() {
            let path = entry.path();
            if path.extension().map(|e| e == "exe").unwrap_or(false) {
                let name = path.file_name()?.to_string_lossy().to_lowercase();
                if name.contains("setup")
                    || name.contains("redist")
                    || name.contains("uninstall")
                    || name.contains("vcredist")
                    || name.contains("installer")
                    || name.contains("crashhandler")
                {
                    continue;
                }
                if let Ok(data) = std::fs::read(&path) {
                    if let Some(info) = parse_pe_imports(&data) {
                        return Some(info);
                    }
                }
            }
        }
    }
    None
}

pub fn read_agility_exports(path: &Path) -> Option<AgilityExports> {
    let data = std::fs::read(path).ok()?;
    parse_agility_exports(&data)
}

pub fn read_export_u32(path: &Path, symbol: &str) -> Option<u32> {
    let data = std::fs::read(path).ok()?;
    parse_export_u32(&data, symbol)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn detects_d3d10_from_public_and_core_imports() {
        assert_eq!(detect_d3d_api(&["d3d10.dll".into()]), D3dApi::D3D10);
        assert_eq!(detect_d3d_api(&["d3d10_1.dll".into()]), D3dApi::D3D10);
        assert_eq!(detect_d3d_api(&["d3d10core.dll".into()]), D3dApi::D3D10);
    }

    #[test]
    fn d3d12_import_takes_priority_over_d3d10_compat_imports() {
        assert_eq!(detect_d3d_api(&["d3d10core.dll".into(), "d3d12.dll".into()]), D3dApi::D3D12);
    }

    #[test]
    fn distinguishes_dotnet_assemblies_from_native_pe() {
        // Build a synthetic PE32+ with a nonzero COM descriptor (index 14).
        let mut managed = vec![0_u8; 0x200];
        managed[0..2].copy_from_slice(b"MZ");
        managed[0x3c..0x40].copy_from_slice(&(0x80_u32).to_le_bytes());
        managed[0x80..0x84].copy_from_slice(b"PE\0\0");
        managed[0x84..0x86].copy_from_slice(&MACHINE_AMD64.to_le_bytes());
        managed[0x98..0x9a].copy_from_slice(&(0x20b_u16).to_le_bytes());
        // Data directories start at pe_offset+24+0x70; COM descriptor index 14.
        let com_entry = 0x80 + 24 + 0x70 + 14 * 8;
        managed[com_entry..com_entry + 4].copy_from_slice(&(0x1000_u32).to_le_bytes());
        assert!(is_dotnet_assembly(&managed), "PE with COM descriptor must classify as .NET");

        // Same image with a zeroed COM descriptor = native executable.
        managed[com_entry..com_entry + 4].copy_from_slice(&0_u32.to_le_bytes());
        assert!(!is_dotnet_assembly(&managed), "native PE must not classify as .NET");

        // Native PE32 (x86 Unity player style, DREDGE.exe) — no COM descriptor.
        let mut native = vec![0_u8; 0x200];
        native[0..2].copy_from_slice(b"MZ");
        native[0x3c..0x40].copy_from_slice(&(0x80_u32).to_le_bytes());
        native[0x80..0x84].copy_from_slice(b"PE\0\0");
        native[0x84..0x86].copy_from_slice(&MACHINE_I386.to_le_bytes());
        native[0x98..0x9a].copy_from_slice(&(0x10b_u16).to_le_bytes());
        assert!(!is_dotnet_assembly(&native));

        assert!(!is_dotnet_assembly(b"not a pe at all"));
    }

    #[test]
    fn parses_exported_agility_symbols_from_synthetic_pe() {
        let mut data = vec![0_u8; 0x1000];
        data[0] = b'M';
        data[1] = b'Z';
        data[0x3c..0x40].copy_from_slice(&(0x80_u32).to_le_bytes());
        data[0x80..0x84].copy_from_slice(b"PE\0\0");
        data[0x84..0x86].copy_from_slice(&MACHINE_AMD64.to_le_bytes());
        data[0x86..0x88].copy_from_slice(&(1_u16).to_le_bytes());
        data[0x94..0x96].copy_from_slice(&(0x00f0_u16).to_le_bytes());
        data[0x98..0x9a].copy_from_slice(&(0x20b_u16).to_le_bytes());
        data[0x108..0x10c].copy_from_slice(&(0x2000_u32).to_le_bytes());
        data[0x10c..0x110].copy_from_slice(&(0x80_u32).to_le_bytes());

        let section = 0x188;
        data[section + 8..section + 12].copy_from_slice(&(0x400_u32).to_le_bytes());
        data[section + 12..section + 16].copy_from_slice(&(0x2000_u32).to_le_bytes());
        data[section + 16..section + 20].copy_from_slice(&(0x400_u32).to_le_bytes());
        data[section + 20..section + 24].copy_from_slice(&(0x200_u32).to_le_bytes());

        let export = 0x200;
        data[export + 20..export + 24].copy_from_slice(&(2_u32).to_le_bytes());
        data[export + 24..export + 28].copy_from_slice(&(2_u32).to_le_bytes());
        data[export + 28..export + 32].copy_from_slice(&(0x2040_u32).to_le_bytes());
        data[export + 32..export + 36].copy_from_slice(&(0x2050_u32).to_le_bytes());
        data[export + 36..export + 40].copy_from_slice(&(0x2060_u32).to_le_bytes());

        data[0x240..0x244].copy_from_slice(&(0x2090_u32).to_le_bytes());
        data[0x248..0x24c].copy_from_slice(&(0x2080_u32).to_le_bytes());
        data[0x244..0x248].copy_from_slice(&(0x20a0_u32).to_le_bytes());
        data[0x250..0x254].copy_from_slice(&(0x2070_u32).to_le_bytes());
        data[0x254..0x258].copy_from_slice(&(0x2080_u32).to_le_bytes());
        data[0x260..0x262].copy_from_slice(&(0_u16).to_le_bytes());
        data[0x262..0x264].copy_from_slice(&(1_u16).to_le_bytes());
        data[0x270..0x27d].copy_from_slice(b"D3D12SDKPath\0");
        data[0x280..0x290].copy_from_slice(b"D3D12SDKVersion\0");
        data[0x290..0x29d].copy_from_slice(b".\\D3D12\\x64\\\0");
        data[0x2a0..0x2a4].copy_from_slice(&(614_u32).to_le_bytes());

        let exports = parse_agility_exports(&data).expect("synthetic exports");
        assert_eq!(exports, AgilityExports { sdk_version: 614, sdk_path: ".\\D3D12\\x64\\".to_string() });
    }
}

use std::fs::{self, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use tiny_http::{Header, Request};

pub const TOKEN_FILE_NAME: &str = ".backend-token";
pub const TOKEN_HEADER: &str = "X-MetalSharp-Token";

const TOKEN_BYTE_LENGTH: usize = 32;

pub struct BackendAuth {
    token: String,
}

impl BackendAuth {
    pub fn create(home: &Path) -> Result<Self, String> {
        let mut bytes = [0u8; TOKEN_BYTE_LENGTH];
        getrandom::fill(&mut bytes).map_err(|error| format!("generate backend token: {error}"))?;
        let token = hex_encode(&bytes);
        write_token_file(&token_path(home), &token)?;
        Ok(Self { token })
    }

    pub fn is_authorized(&self, request: &Request) -> bool {
        self.is_authorized_headers(request.headers())
    }

    fn is_authorized_headers(&self, headers: &[Header]) -> bool {
        headers
            .iter()
            .filter(|header| header.field.equiv(TOKEN_HEADER))
            .any(|header| token_matches(&self.token, header.value.as_str()))
    }
}

pub fn token_path(home: &Path) -> PathBuf {
    home.join(TOKEN_FILE_NAME)
}

fn token_matches(expected: &str, provided: &str) -> bool {
    let expected = expected.as_bytes();
    let provided = provided.as_bytes();
    let length = expected.len().max(provided.len());
    let mut difference = expected.len() ^ provided.len();

    for index in 0..length {
        let expected_byte = expected.get(index).copied().unwrap_or(0);
        let provided_byte = provided.get(index).copied().unwrap_or(0);
        difference |= usize::from(expected_byte ^ provided_byte);
    }

    difference == 0
}

fn hex_encode(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut encoded = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        encoded.push(HEX[(byte >> 4) as usize] as char);
        encoded.push(HEX[(byte & 0x0f) as usize] as char);
    }
    encoded
}

fn write_token_file(path: &Path, token: &str) -> Result<(), String> {
    let parent = path.parent().ok_or_else(|| format!("backend token path has no parent: {}", path.display()))?;
    fs::create_dir_all(parent).map_err(|error| format!("create backend token directory: {error}"))?;

    let timestamp =
        SystemTime::now().duration_since(UNIX_EPOCH).map(|duration| duration.as_nanos()).unwrap_or_default();
    let temporary_path = parent.join(format!(".{TOKEN_FILE_NAME}.{}.{}.tmp", std::process::id(), timestamp));

    let result = (|| {
        let mut options = OpenOptions::new();
        options.create_new(true).write(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            options.mode(0o600);
        }

        let mut file = options.open(&temporary_path).map_err(|error| format!("create backend token file: {error}"))?;
        file.write_all(token.as_bytes())
            .and_then(|_| file.write_all(b"\n"))
            .and_then(|_| file.sync_all())
            .map_err(|error| format!("write backend token file: {error}"))?;
        drop(file);

        #[cfg(unix)]
        fs::set_permissions(&temporary_path, std::os::unix::fs::PermissionsExt::from_mode(0o600))
            .map_err(|error| format!("secure backend token file: {error}"))?;

        fs::rename(&temporary_path, path).map_err(|error| format!("publish backend token file: {error}"))?;

        Ok(())
    })();

    if result.is_err() {
        let _ = fs::remove_file(&temporary_path);
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    #[cfg(unix)]
    use std::os::unix::fs::PermissionsExt;

    fn test_home(name: &str) -> PathBuf {
        let timestamp = SystemTime::now().duration_since(UNIX_EPOCH).expect("system clock").as_nanos();
        std::env::temp_dir().join(format!("metalsharp-backend-auth-{name}-{}-{timestamp}", std::process::id()))
    }

    #[test]
    fn creates_a_random_hex_token_with_private_permissions() {
        let home = test_home("format");
        let auth = BackendAuth::create(&home).expect("create backend auth");
        let token = fs::read_to_string(token_path(&home)).expect("read backend token");
        let token = token.trim();

        assert_eq!(token.len(), TOKEN_BYTE_LENGTH * 2);
        assert!(token.bytes().all(|byte| byte.is_ascii_hexdigit()));
        assert!(token_matches(token, token));
        assert!(auth.is_authorized_headers(&[Header::from_bytes(TOKEN_HEADER, token).expect("token header"),]));

        #[cfg(unix)]
        assert_eq!(fs::metadata(token_path(&home)).expect("token metadata").permissions().mode() & 0o777, 0o600);

        let _ = fs::remove_dir_all(home);
    }

    #[test]
    fn rejects_missing_wrong_and_modified_tokens() {
        assert!(token_matches("abcdef", "abcdef"));
        assert!(!token_matches("abcdef", "abcde0"));
        assert!(!token_matches("abcdef", "abcdef0"));
        assert!(!token_matches("abcdef", ""));
    }

    #[test]
    fn accepts_only_the_backend_token_header() {
        let home = test_home("headers");
        let auth = BackendAuth::create(&home).expect("create backend auth");
        let token = fs::read_to_string(token_path(&home)).expect("read backend token");
        let token = token.trim();

        assert!(!auth.is_authorized_headers(&[]));
        assert!(!auth.is_authorized_headers(&[Header::from_bytes(TOKEN_HEADER, "wrong").expect("wrong token header"),]));
        assert!(!auth.is_authorized_headers(&[Header::from_bytes("Authorization", token).expect("wrong header name"),]));
        assert!(auth.is_authorized_headers(&[Header::from_bytes(TOKEN_HEADER, token).expect("valid token header"),]));

        let _ = fs::remove_dir_all(home);
    }
}

//! ureq transport for the C backend's owner-only Unix socket
//! (`<METALSHARP_HOME>/run/backend.sock`). Every request goes to that socket
//! whatever the URL host, so no name is resolved and no TCP port is opened.
use std::{
    io::{self, Read, Write},
    net::{Ipv4Addr, SocketAddr},
    os::unix::net::UnixStream,
    path::{Path, PathBuf},
    sync::Arc,
    time::Duration,
};
use ureq::{
    Agent, Error,
    config::Config,
    http::Uri,
    unversioned::{
        resolver::{ResolvedSocketAddrs, Resolver},
        transport::{Buffers, ConnectionDetails, Connector, LazyBuffers, NextTimeout, Transport},
    },
};

/// URL base for backend requests; the host is never resolved.
pub const BASE_URL: &str = "http://localhost";

/// Agent whose every connection is a fresh stream to `socket`.
pub fn agent(socket: &Path, timeout: Duration) -> Agent {
    let config = Agent::config_builder()
        .timeout_global(Some(timeout))
        .proxy(None)
        .max_redirects(0)
        .http_status_as_error(false)
        .build();
    Agent::with_parts(
        config,
        UnixConnector(Arc::from(socket.to_path_buf())),
        NoResolver,
    )
}

#[derive(Debug)]
struct NoResolver;
impl Resolver for NoResolver {
    fn resolve(
        &self,
        _uri: &Uri,
        _config: &Config,
        _timeout: NextTimeout,
    ) -> Result<ResolvedSocketAddrs, Error> {
        // ureq requires at least one address; the connector ignores it.
        let mut addrs = self.empty();
        addrs.push(SocketAddr::from((Ipv4Addr::LOCALHOST, 0)));
        Ok(addrs)
    }
}

#[derive(Debug)]
struct UnixConnector(Arc<PathBuf>);
impl<In: Transport> Connector<In> for UnixConnector {
    type Out = UnixTransport;
    fn connect(
        &self,
        details: &ConnectionDetails,
        _chained: Option<In>,
    ) -> Result<Option<Self::Out>, Error> {
        let stream = UnixStream::connect(self.0.as_path())?;
        let buffers = LazyBuffers::new(
            details.config.input_buffer_size(),
            details.config.output_buffer_size(),
        );
        Ok(Some(UnixTransport { stream, buffers }))
    }
}

#[derive(Debug)]
struct UnixTransport {
    stream: UnixStream,
    buffers: LazyBuffers,
}
/// macOS rejects SO_RCVTIMEO/SO_SNDTIMEO with EINVAL once the peer has closed
/// its end, which the backend does right after writing a response. Buffered
/// response bytes are still readable then, so only other failures are errors.
fn set_timeout(
    stream: &UnixStream,
    timeout: NextTimeout,
    set: fn(&UnixStream, Option<Duration>) -> io::Result<()>,
) -> io::Result<()> {
    match set(stream, timeout.not_zero().map(|t| *t)) {
        Err(error) if error.kind() == io::ErrorKind::InvalidInput => Ok(()),
        result => result,
    }
}
fn timed_out(error: &io::Error) -> bool {
    matches!(
        error.kind(),
        io::ErrorKind::TimedOut | io::ErrorKind::WouldBlock
    )
}
impl Transport for UnixTransport {
    fn buffers(&mut self) -> &mut dyn Buffers {
        &mut self.buffers
    }
    fn transmit_output(&mut self, amount: usize, timeout: NextTimeout) -> Result<(), Error> {
        set_timeout(&self.stream, timeout, UnixStream::set_write_timeout)?;
        let output = &self.buffers.output()[..amount];
        self.stream.write_all(output).map_err(|error| {
            if timed_out(&error) {
                Error::Timeout(timeout.reason)
            } else {
                error.into()
            }
        })
    }
    fn await_input(&mut self, timeout: NextTimeout) -> Result<bool, Error> {
        set_timeout(&self.stream, timeout, UnixStream::set_read_timeout)?;
        let input = self.buffers.input_append_buf();
        let amount = self.stream.read(input).map_err(|error| {
            if timed_out(&error) {
                Error::Timeout(timeout.reason)
            } else {
                Error::from(error)
            }
        })?;
        self.buffers.input_appended(amount);
        Ok(amount > 0)
    }
    fn is_open(&mut self) -> bool {
        // The backend closes every connection after one response.
        let mut probe = [0u8; 1];
        if self.stream.set_nonblocking(true).is_err() {
            return false;
        }
        let open =
            matches!(self.stream.read(&mut probe), Err(e) if e.kind() == io::ErrorKind::WouldBlock);
        open && self.stream.set_nonblocking(false).is_ok()
    }
}

/// Listener on a fresh socket path for request/response fixtures.
#[cfg(test)]
pub fn test_listener() -> (std::os::unix::net::UnixListener, PathBuf) {
    use std::sync::atomic::{AtomicUsize, Ordering};
    static NEXT: AtomicUsize = AtomicUsize::new(0);
    let path = std::env::temp_dir().join(format!(
        "ms-gpui-{}-{}.sock",
        std::process::id(),
        NEXT.fetch_add(1, Ordering::Relaxed)
    ));
    let _ = std::fs::remove_file(&path);
    (std::os::unix::net::UnixListener::bind(&path).unwrap(), path)
}

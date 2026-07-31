# Security Policy

## Supported versions

Security fixes are applied to the latest released version of MetalSharp. The
`main` branch may contain unreleased changes and is supported on a best-effort
basis. Older releases are not supported; users should upgrade before reporting
an issue that may already be fixed.

| Version        | Supported   |
| -------------- | ----------- |
| Latest release | Yes         |
| `main`         | Best effort |
| Older releases | No          |

## Reporting a vulnerability

Please do not open a public issue, discussion, or pull request for a suspected
security vulnerability.

Use GitHub's private vulnerability reporting form:

https://github.com/metalsharp/MetalSharp/security/advisories/new

Include as much of the following as possible:

- the affected MetalSharp version or commit;
- the affected macOS version and Apple Silicon model;
- a clear description of the impact and attack scenario;
- steps or a minimal proof of concept that reproduce the issue;
- relevant logs, crash reports, or screenshots with secrets and personal data
  removed; and
- any known mitigations or suggested fixes.

The maintainers aim to acknowledge complete reports within five business days.
After reproducing and assessing the issue, they will coordinate remediation and
disclosure with the reporter. Timelines depend on severity and the complexity
of safely distributing a fix.

Please allow a reasonable remediation period before publishing details. The
project will credit reporters who request attribution, unless legal or privacy
constraints prevent it.

## Scope

Reports are in scope when they concern MetalSharp-owned code, release artifacts,
update or installation behavior, runtime isolation, credential or secret
exposure, or a dependency vulnerability with a demonstrated impact on
MetalSharp.

Game-specific bugs, compatibility problems without a security impact, and
vulnerabilities in third-party software that MetalSharp does not distribute or
control should be reported to the appropriate issue tracker or upstream vendor.

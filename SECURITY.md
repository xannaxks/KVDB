
# Security Policy

KVDB is an experimental storage engine and is not currently production-ready.

## Supported Versions

Security updates are only considered for the latest version of the main branch.

| Version | Supported |
| ------- | --------- |
| main    | Yes       |
| older commits | No |

## Reporting a Vulnerability

If you find a security issue, please do not open a public GitHub issue.

Instead, contact the maintainer privately:

- Email: your-email@example.com

Please include:

- A description of the issue
- Steps to reproduce it
- Possible impact
- Suggested fix, if known

## Scope

Relevant issues may include:

- Data corruption
- Unsafe file handling
- Memory safety bugs
- Crash recovery bugs
- Incorrect checksum validation
- Path handling issues
- Undefined behavior that may affect stored data

## Out of Scope

The following are currently out of scope:

- Production hardening
- Network security
- Authentication/authorization
- Encryption
- Multi-user access control

KVDB currently focuses on local storage engine internals, not server-side
security or distributed database security.
# Privacy and publication

LibreDesk is self-hosted. Each installation needs its own follower account,
bridge hostname, network configuration, and unrelated per-device and ingestion
tokens. There is no shared hosted service or public credential.

## Keep private

Store live configuration outside the repository, restrict its permissions, and
never attach it to an issue or pull request. This includes LibreLinkUp passwords,
Wi-Fi credentials, device and ingestion tokens or hashes, Redis credentials,
tunnel credentials, and installer capability URLs.

Device-specific firmware embeds private configuration. Build and flash it
locally; do not upload `.bin`, `.elf`, build directories, or firmware archives
as public releases. Logs, coredumps, patient identifiers, real readings, and
screenshots from a live device also remain private.

The committed JSON fixtures and screen gallery are synthetic. Keep future
fixtures synthetic as well. Preserve third-party license files when modifying
the vendored flasher.

## Before contributing

Install Gitleaks from its official distribution, then run:

```sh
./scripts/install-hooks.sh
python3 scripts/check-public-files.py
gitleaks dir . --redact
gitleaks git . --redact
```

The filename check inspects tracked files and rejects private configuration,
firmware, archives, and runtime data. Gitleaks scans for secret patterns. These
checks complement review of examples, documentation, and image content; they
cannot prove that arbitrary personal information is absent.

## Reporting a problem

Open a public issue only with synthetic reproductions and redacted diagnostics.
Never post a credential, patient identifier, real reading, or live configuration.
If a credential is exposed, revoke it at its provider and replace it before
continuing to use the installation. Removing a file from the latest commit
does not remove it from Git history.

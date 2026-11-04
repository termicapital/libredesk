#!/usr/bin/env python3
from __future__ import annotations

import argparse
import ctypes
import errno
import os
import secrets
import signal
import stat
import sys
from pathlib import Path
from typing import NoReturn

from secure_render_lock import UnsafeLockError, open_directory_no_symlinks


class PublishError(RuntimeError):
    pass


class TargetChangedError(PublishError):
    pass


_BLOCKED_SIGNALS = {signal.SIGINT, signal.SIGTERM}
_RENAME_NOREPLACE = 1
_LIBC = ctypes.CDLL(None, use_errno=True)
_RENAMEAT2 = getattr(_LIBC, "renameat2", None)
if _RENAMEAT2 is not None:
    _RENAMEAT2.argtypes = [
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_uint,
    ]
    _RENAMEAT2.restype = ctypes.c_int


def _absolute(path: Path) -> Path:
    return Path(os.path.abspath(os.fspath(path)))


def _rename_noreplace(
    source_parent: int,
    source_name: str,
    destination_parent: int,
    destination_name: str,
) -> None:
    if _RENAMEAT2 is None:
        raise PublishError("secure publication requires renameat2(RENAME_NOREPLACE)")
    result = _RENAMEAT2(
        source_parent,
        os.fsencode(source_name),
        destination_parent,
        os.fsencode(destination_name),
        _RENAME_NOREPLACE,
    )
    if result == 0:
        return
    error_number = ctypes.get_errno()
    if error_number in {errno.ENOSYS, errno.EINVAL, errno.ENOTSUP}:
        raise PublishError(
            "secure publication requires kernel renameat2(RENAME_NOREPLACE) support"
        )
    raise OSError(
        error_number,
        os.strerror(error_number),
        f"{source_name} -> {destination_name}",
    )


def _rename_expected_noreplace(
    source_parent: int,
    source_name: str,
    destination_parent: int,
    destination_name: str,
    expected_descriptor: int,
    label: str,
) -> None:
    _rename_noreplace(
        source_parent,
        source_name,
        destination_parent,
        destination_name,
    )
    try:
        _validate_named_directory_descriptor_at(
            destination_parent,
            destination_name,
            expected_descriptor,
            label,
        )
    except BaseException:
        try:
            _rename_noreplace(
                destination_parent,
                destination_name,
                source_parent,
                source_name,
            )
        except BaseException:
            pass
        raise


def _open_owned_directory(path: Path, label: str) -> int:
    try:
        descriptor = open_directory_no_symlinks(path)
    except UnsafeLockError as error:
        raise PublishError(str(error)) from error
    try:
        _validate_owned_directory_descriptor(descriptor, path, label)
    except BaseException:
        os.close(descriptor)
        raise
    return descriptor


def _open_directory_at(parent_descriptor: int, name: str) -> int:
    return os.open(
        name,
        os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC,
        dir_fd=parent_descriptor,
    )


def _same_entry(left: os.stat_result, right: os.stat_result) -> bool:
    return (
        left.st_dev,
        left.st_ino,
        left.st_uid,
        stat.S_IMODE(left.st_mode),
        left.st_nlink,
    ) == (
        right.st_dev,
        right.st_ino,
        right.st_uid,
        stat.S_IMODE(right.st_mode),
        right.st_nlink,
    )


def _validate_owned_directory_descriptor(
    descriptor: int, path: Path, label: str
) -> None:
    opened_before = os.fstat(descriptor)
    try:
        current_before = os.stat(_absolute(path), follow_symlinks=False)
        opened_after = os.fstat(descriptor)
        current_after = os.stat(_absolute(path), follow_symlinks=False)
    except OSError as error:
        raise PublishError(f"{label} path changed: {path}") from error
    if stat.S_ISLNK(current_before.st_mode) or stat.S_ISLNK(current_after.st_mode):
        raise PublishError(f"{label} is a symbolic link: {path}")
    if not stat.S_ISDIR(current_before.st_mode) or not stat.S_ISDIR(
        current_after.st_mode
    ):
        raise PublishError(f"{label} is not a directory: {path}")
    if (
        not _same_entry(opened_before, opened_after)
        or not _same_entry(current_before, current_after)
        or not _same_entry(opened_after, current_after)
    ):
        raise PublishError(f"{label} path changed: {path}")
    if opened_after.st_uid != os.geteuid():
        raise PublishError(f"{label} is not owned by the current user: {path}")
    if stat.S_IMODE(opened_after.st_mode) & 0o022:
        raise PublishError(f"{label} permits group/world writes: {path}")


def _validate_named_directory_descriptor_at(
    parent_descriptor: int, name: str, descriptor: int, label: str
) -> None:
    path_before = os.stat(name, dir_fd=parent_descriptor, follow_symlinks=False)
    opened_before = os.fstat(descriptor)
    path_after = os.stat(name, dir_fd=parent_descriptor, follow_symlinks=False)
    opened_after = os.fstat(descriptor)
    if (
        stat.S_ISLNK(path_before.st_mode)
        or stat.S_ISLNK(path_after.st_mode)
        or not stat.S_ISDIR(path_before.st_mode)
        or not stat.S_ISDIR(path_after.st_mode)
    ):
        raise PublishError(f"{label} is not a direct directory")
    if (
        not _same_entry(path_before, path_after)
        or not _same_entry(path_after, opened_before)
        or not _same_entry(opened_before, opened_after)
    ):
        raise PublishError(f"{label} changed while it was open")
    if opened_after.st_uid != os.geteuid():
        raise PublishError(f"{label} is not owned by the current user")
    if stat.S_IMODE(opened_after.st_mode) & 0o022:
        raise PublishError(f"{label} permits group/world writes")


def _validate_flat_directory_at(parent_descriptor: int, name: str, label: str) -> None:
    try:
        path_metadata = os.stat(name, dir_fd=parent_descriptor, follow_symlinks=False)
    except FileNotFoundError:
        raise
    if stat.S_ISLNK(path_metadata.st_mode):
        raise PublishError(f"{label} is a symbolic link")
    if not stat.S_ISDIR(path_metadata.st_mode):
        raise PublishError(f"{label} is not a directory")
    descriptor = _open_directory_at(parent_descriptor, name)
    try:
        _validate_named_directory_descriptor_at(
            parent_descriptor, name, descriptor, label
        )
        for entry_name in os.listdir(descriptor):
            entry = os.stat(entry_name, dir_fd=descriptor, follow_symlinks=False)
            if stat.S_ISLNK(entry.st_mode):
                raise PublishError(f"{label} contains a symbolic link: {entry_name}")
            if not stat.S_ISREG(entry.st_mode):
                raise PublishError(f"{label} contains a non-regular entry: {entry_name}")
            if entry.st_uid != os.geteuid():
                raise PublishError(f"{label} contains an unowned entry: {entry_name}")
            if entry.st_nlink != 1:
                raise PublishError(f"{label} contains a hard link: {entry_name}")
        _validate_named_directory_descriptor_at(
            parent_descriptor, name, descriptor, label
        )
    finally:
        os.close(descriptor)


def _write_all(descriptor: int, payload: bytes) -> None:
    offset = 0
    while offset < len(payload):
        written = os.write(descriptor, payload[offset:])
        if written <= 0:
            raise PublishError("short write while staging publication")
        offset += written


def _copy_candidate_files(candidate_descriptor: int, stage_descriptor: int) -> None:
    entry_names = sorted(os.listdir(candidate_descriptor))
    if not entry_names:
        raise PublishError("publication candidate is empty")
    for entry_name in entry_names:
        before = os.stat(entry_name, dir_fd=candidate_descriptor, follow_symlinks=False)
        if stat.S_ISLNK(before.st_mode):
            raise PublishError(f"publication candidate contains a symbolic link: {entry_name}")
        if not stat.S_ISREG(before.st_mode):
            raise PublishError(f"publication candidate contains a non-regular entry: {entry_name}")
        if before.st_uid != os.geteuid():
            raise PublishError(f"publication candidate contains an unowned entry: {entry_name}")
        if before.st_nlink != 1:
            raise PublishError(f"publication candidate contains a hard link: {entry_name}")

        source = os.open(
            entry_name,
            os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC,
            dir_fd=candidate_descriptor,
        )
        destination = -1
        try:
            opened = os.fstat(source)
            if not _same_entry(before, opened):
                raise PublishError(f"publication candidate entry changed: {entry_name}")
            destination = os.open(
                entry_name,
                os.O_WRONLY
                | os.O_CREAT
                | os.O_EXCL
                | os.O_NOFOLLOW
                | os.O_CLOEXEC,
                0o600,
                dir_fd=stage_descriptor,
            )
            while True:
                block = os.read(source, 1024 * 1024)
                if not block:
                    break
                _write_all(destination, block)
            after = os.fstat(source)
            current = os.stat(entry_name, dir_fd=candidate_descriptor, follow_symlinks=False)
            if not _same_entry(opened, after) or not _same_entry(after, current):
                raise PublishError(f"publication candidate entry changed while copying: {entry_name}")
            if (
                after.st_size != before.st_size
                or after.st_mtime_ns != before.st_mtime_ns
                or after.st_ctime_ns != before.st_ctime_ns
            ):
                raise PublishError(f"publication candidate content changed while copying: {entry_name}")
            os.fchmod(destination, 0o644)
            os.fsync(destination)
            published = os.fstat(destination)
            if not stat.S_ISREG(published.st_mode) or published.st_nlink != 1:
                raise PublishError(f"staged artifact is not a single-link regular file: {entry_name}")
        finally:
            if destination >= 0:
                os.close(destination)
            os.close(source)
    os.fsync(stage_descriptor)


def _create_transaction(parent_descriptor: int) -> tuple[str, int, int]:
    for _ in range(128):
        name = f".screens-publish.{secrets.token_hex(16)}"
        try:
            os.mkdir(name, 0o700, dir_fd=parent_descriptor)
        except FileExistsError:
            continue
        transaction = _open_directory_at(parent_descriptor, name)
        _validate_named_directory_descriptor_at(
            parent_descriptor, name, transaction, "publication transaction"
        )
        os.mkdir("new", 0o700, dir_fd=transaction)
        stage = _open_directory_at(transaction, "new")
        _validate_named_directory_descriptor_at(
            transaction, "new", stage, "publication stage"
        )
        return name, transaction, stage
    raise PublishError("cannot allocate a private publication transaction")


def _validate_guard(
    guard: tuple[int, str, int, str] | None,
) -> None:
    if guard is None:
        return
    parent_descriptor, name, expected_descriptor, label = guard
    try:
        _validate_named_directory_descriptor_at(
            parent_descriptor, name, expected_descriptor, label
        )
    except (OSError, PublishError) as error:
        raise TargetChangedError(
            f"{label} changed while protected cleanup was running"
        ) from error


def _move_expected_to_quarantine(
    parent_descriptor: int,
    name: str,
    expected_descriptor: int,
    label: str,
) -> str:
    for _ in range(128):
        quarantine = f".screens-quarantine.{secrets.token_hex(16)}"
        try:
            _rename_noreplace(
                parent_descriptor,
                name,
                parent_descriptor,
                quarantine,
            )
        except OSError as error:
            if error.errno == errno.EEXIST:
                continue
            raise
        try:
            _validate_named_directory_descriptor_at(
                parent_descriptor,
                quarantine,
                expected_descriptor,
                label,
            )
        except BaseException:
            try:
                _rename_noreplace(
                    parent_descriptor,
                    quarantine,
                    parent_descriptor,
                    name,
                )
            except BaseException:
                pass
            raise
        return quarantine
    raise PublishError(f"cannot allocate quarantine for {label}")


def _quarantine_directory_at(
    parent_descriptor: int,
    name: str,
    *,
    expected_descriptor: int,
    guard: tuple[int, str, int, str] | None = None,
) -> None:
    _validate_guard(guard)
    quarantine = _move_expected_to_quarantine(
        parent_descriptor,
        name,
        expected_descriptor,
        "rollback directory",
    )
    _validate_guard(guard)
    _validate_named_directory_descriptor_at(
        parent_descriptor,
        quarantine,
        expected_descriptor,
        "quarantined rollback directory",
    )
    # Retain the complete transaction-owned generation. Deleting children by
    # name would create a stat/unlink race in which a swapped entry could be
    # deleted even though its parent directory descriptor remained trusted.


def _quarantine_transaction(
    parent_descriptor: int,
    name: str,
    transaction_descriptor: int,
    *,
    guard: tuple[int, str, int, str] | None = None,
) -> None:
    _validate_guard(guard)
    quarantine = _move_expected_to_quarantine(
        parent_descriptor,
        name,
        transaction_descriptor,
        "publication transaction",
    )
    _validate_guard(guard)
    _validate_named_directory_descriptor_at(
        parent_descriptor,
        quarantine,
        transaction_descriptor,
        "quarantined publication transaction",
    )
    # The quarantined transaction, including any complete previous generation,
    # is intentionally retained: no destructive pathname operation is safe
    # after the final retained-descriptor identity check.


def _warn_cleanup(transaction_name: str, error: BaseException) -> None:
    print(
        f"warning: published screens are valid but private cleanup remains at "
        f"{transaction_name}: {error}",
        file=sys.stderr,
    )


def publish_directory(
    candidate: Path,
    target: Path,
    *,
    fault_after_backup: bool = False,
) -> None:
    if not hasattr(signal, "pthread_sigmask"):
        raise PublishError("secure publication requires pthread signal masking")

    candidate_absolute = _absolute(candidate)
    target_absolute = _absolute(target)
    if target_absolute.name in {"", ".", ".."}:
        raise PublishError("publication target must be a named directory")

    candidate_descriptor = _open_owned_directory(candidate_absolute, "publication candidate")
    parent_descriptor = _open_owned_directory(target_absolute.parent, "publication parent")
    transaction_name = ""
    transaction_descriptor = -1
    stage_descriptor = -1
    backup_descriptor = -1
    try:
        transaction_name, transaction_descriptor, stage_descriptor = _create_transaction(
            parent_descriptor
        )
        _copy_candidate_files(candidate_descriptor, stage_descriptor)
        _validate_owned_directory_descriptor(
            candidate_descriptor, candidate_absolute, "publication candidate"
        )
        os.fchmod(stage_descriptor, 0o755)
        os.fsync(stage_descriptor)
        _validate_named_directory_descriptor_at(
            transaction_descriptor, "new", stage_descriptor, "publication stage"
        )

        target_exists = True
        try:
            _validate_flat_directory_at(
                parent_descriptor, target_absolute.name, "existing publication target"
            )
        except FileNotFoundError:
            target_exists = False
        if target_exists:
            backup_descriptor = _open_directory_at(
                parent_descriptor, target_absolute.name
            )
            _validate_named_directory_descriptor_at(
                parent_descriptor,
                target_absolute.name,
                backup_descriptor,
                "existing publication target",
            )

        _validate_owned_directory_descriptor(
            parent_descriptor, target_absolute.parent, "publication parent"
        )
        _validate_named_directory_descriptor_at(
            parent_descriptor,
            transaction_name,
            transaction_descriptor,
            "publication transaction",
        )
        previous_mask = signal.pthread_sigmask(signal.SIG_BLOCK, _BLOCKED_SIGNALS)
        backup_created = False
        installed = False
        committed = False
        try:
            _validate_owned_directory_descriptor(
                parent_descriptor, target_absolute.parent, "publication parent"
            )
            _validate_named_directory_descriptor_at(
                parent_descriptor,
                transaction_name,
                transaction_descriptor,
                "publication transaction",
            )
            if target_exists:
                _validate_named_directory_descriptor_at(
                    parent_descriptor,
                    target_absolute.name,
                    backup_descriptor,
                    "existing publication target",
                )
                _rename_expected_noreplace(
                    parent_descriptor,
                    target_absolute.name,
                    transaction_descriptor,
                    "backup",
                    backup_descriptor,
                    "publication backup",
                )
                backup_created = True
                _validate_named_directory_descriptor_at(
                    transaction_descriptor,
                    "backup",
                    backup_descriptor,
                    "publication backup",
                )
            if fault_after_backup:
                raise PublishError("injected publication failure after backup")
            _rename_expected_noreplace(
                transaction_descriptor,
                "new",
                parent_descriptor,
                target_absolute.name,
                stage_descriptor,
                "installed publication target",
            )
            installed = True
            _validate_named_directory_descriptor_at(
                parent_descriptor,
                target_absolute.name,
                stage_descriptor,
                "installed publication target",
            )
            _validate_flat_directory_at(
                parent_descriptor,
                target_absolute.name,
                "installed publication target",
            )
            _validate_owned_directory_descriptor(
                parent_descriptor, target_absolute.parent, "publication parent"
            )
            os.fsync(parent_descriptor)
            committed = True
        except BaseException as error:
            try:
                if installed:
                    try:
                        os.stat(
                            target_absolute.name,
                            dir_fd=parent_descriptor,
                            follow_symlinks=False,
                        )
                    except FileNotFoundError:
                        installed = False
                    else:
                        _quarantine_directory_at(
                            parent_descriptor,
                            target_absolute.name,
                            expected_descriptor=stage_descriptor,
                        )
                        installed = False
                if backup_created:
                    _validate_named_directory_descriptor_at(
                        transaction_descriptor,
                        "backup",
                        backup_descriptor,
                        "publication backup",
                    )
                    _rename_expected_noreplace(
                        transaction_descriptor,
                        "backup",
                        parent_descriptor,
                        target_absolute.name,
                        backup_descriptor,
                        "restored publication target",
                    )
                    _validate_named_directory_descriptor_at(
                        parent_descriptor,
                        target_absolute.name,
                        backup_descriptor,
                        "restored publication target",
                    )
                    backup_created = False
                os.fsync(parent_descriptor)
            except BaseException as rollback_error:
                raise PublishError(
                    f"publication failed and rollback also failed: {rollback_error}"
                ) from error
            if isinstance(error, PublishError):
                raise
            raise PublishError(f"publication failed: {error}") from error
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, previous_mask)

        if not committed:
            raise PublishError("publication transaction did not commit")

        _validate_named_directory_descriptor_at(
            parent_descriptor,
            target_absolute.name,
            stage_descriptor,
            "committed publication target",
        )
        target_guard = (
            parent_descriptor,
            target_absolute.name,
            stage_descriptor,
            "committed publication target",
        )
        cleanup_error: BaseException | None = None
        try:
            if backup_created:
                _validate_named_directory_descriptor_at(
                    transaction_descriptor,
                    "backup",
                    backup_descriptor,
                    "retained publication backup",
                )
            _quarantine_transaction(
                parent_descriptor,
                transaction_name,
                transaction_descriptor,
                guard=target_guard,
            )
            transaction_name = ""
            backup_created = False
        except TargetChangedError:
            raise
        except BaseException as error:
            cleanup_error = error
        if cleanup_error is not None:
            _warn_cleanup(transaction_name, cleanup_error)
    except BaseException:
        if transaction_descriptor >= 0 and transaction_name:
            try:
                if "new" in os.listdir(transaction_descriptor):
                    _quarantine_directory_at(
                        transaction_descriptor,
                        "new",
                        expected_descriptor=stage_descriptor,
                    )
                if "backup" in os.listdir(transaction_descriptor):
                    raise PublishError(
                        "publication transaction retains a recoverable backup"
                    )
                _quarantine_transaction(
                    parent_descriptor, transaction_name, transaction_descriptor
                )
                transaction_name = ""
            except BaseException:
                pass
        raise
    finally:
        if backup_descriptor >= 0:
            os.close(backup_descriptor)
        if stage_descriptor >= 0:
            os.close(stage_descriptor)
        if transaction_descriptor >= 0:
            os.close(transaction_descriptor)
        os.close(parent_descriptor)
        os.close(candidate_descriptor)


def _raise_on_signal(signum: int, _frame: object) -> NoReturn:
    raise PublishError(f"publication interrupted by signal {signum}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Publish verified Libre Desk screens")
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--target", type=Path, required=True)
    arguments = parser.parse_args()
    previous_handlers = {
        signum: signal.signal(signum, _raise_on_signal)
        for signum in _BLOCKED_SIGNALS
    }
    try:
        publish_directory(arguments.candidate, arguments.target)
    except (OSError, PublishError) as error:
        print(str(error), file=sys.stderr)
        return 74
    finally:
        for signum, handler in previous_handlers.items():
            signal.signal(signum, handler)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

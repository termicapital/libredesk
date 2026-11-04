#!/usr/bin/env python3
from __future__ import annotations

import errno
import fcntl
import os
import stat
import sys
from pathlib import Path
from typing import NoReturn

UNSAFE_LOCK_EXIT = 73
LOCK_BUSY_EXIT = 75
LOCK_HELD_ENV = "LIBRE_DESK_RENDER_LOCK_HELD"
LOCK_FD_ENV = "LIBRE_DESK_RENDER_LOCK_FD"
REPOSITORY_FD_ENV = "LIBRE_DESK_RENDER_REPOSITORY_FD"


class UnsafeLockError(ValueError):
    pass


def _fail(message: str, exit_code: int) -> NoReturn:
    print(message, file=sys.stderr)
    raise SystemExit(exit_code)


def open_directory_no_symlinks(path: Path) -> int:
    """Open an absolute directory path one component at a time without following links."""
    if not hasattr(os, "O_NOFOLLOW") or not hasattr(os, "O_DIRECTORY"):
        raise UnsafeLockError("secure directory opening requires O_NOFOLLOW and O_DIRECTORY")
    absolute = Path(os.path.abspath(os.fspath(path)))
    descriptor = os.open(
        os.path.sep,
        os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC,
    )
    try:
        for component in absolute.parts[1:]:
            try:
                child = os.open(
                    component,
                    os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC,
                    dir_fd=descriptor,
                )
            except OSError as error:
                if error.errno in {errno.ELOOP, errno.ENOTDIR}:
                    raise UnsafeLockError(
                        f"repository path contains a symbolic link: {absolute}"
                    ) from error
                raise UnsafeLockError(
                    f"cannot securely open repository directory {absolute}: {error}"
                ) from error
            os.close(descriptor)
            descriptor = child
    except BaseException:
        os.close(descriptor)
        raise
    try:
        opened_before = os.fstat(descriptor)
        current_before = os.stat(absolute, follow_symlinks=False)
        opened_after = os.fstat(descriptor)
        current_after = os.stat(absolute, follow_symlinks=False)
    except OSError as error:
        os.close(descriptor)
        raise UnsafeLockError(f"directory path changed while opening: {absolute}") from error
    if stat.S_ISLNK(current_before.st_mode) or stat.S_ISLNK(current_after.st_mode):
        os.close(descriptor)
        raise UnsafeLockError(f"repository path contains a symbolic link: {absolute}")
    if not stat.S_ISDIR(current_before.st_mode) or not stat.S_ISDIR(current_after.st_mode):
        os.close(descriptor)
        raise UnsafeLockError(f"repository path is not a directory: {absolute}")
    if (
        _metadata_tuple(opened_before) != _metadata_tuple(opened_after)
        or _metadata_tuple(current_before) != _metadata_tuple(current_after)
        or (current_after.st_dev, current_after.st_ino)
        != (opened_after.st_dev, opened_after.st_ino)
    ):
        os.close(descriptor)
        raise UnsafeLockError(f"directory path changed while opening: {absolute}")
    return descriptor


def _metadata_tuple(metadata: os.stat_result) -> tuple[int, int, int, int, int]:
    return (
        metadata.st_dev,
        metadata.st_ino,
        metadata.st_uid,
        stat.S_IMODE(metadata.st_mode),
        metadata.st_nlink,
    )


def _validate_descriptor(descriptor: int, path: Path, label: str) -> None:
    absolute = Path(os.path.abspath(os.fspath(path)))
    try:
        opened_before = os.fstat(descriptor)
    except OSError as error:
        raise UnsafeLockError(f"{label} descriptor is invalid: {error}") from error
    if not stat.S_ISDIR(opened_before.st_mode):
        raise UnsafeLockError(f"{label} is not a directory: {absolute}")
    if opened_before.st_uid != os.geteuid():
        raise UnsafeLockError(f"{label} is not owned by the current user: {absolute}")
    if stat.S_IMODE(opened_before.st_mode) & 0o022:
        raise UnsafeLockError(
            f"{label} permissions permit group/world writes: {absolute}"
        )

    try:
        current_before = os.stat(absolute, follow_symlinks=False)
        opened_after = os.fstat(descriptor)
        current_after = os.stat(absolute, follow_symlinks=False)
    except OSError as error:
        raise UnsafeLockError(f"{label} path changed: {absolute}") from error
    if stat.S_ISLNK(current_before.st_mode) or stat.S_ISLNK(current_after.st_mode):
        raise UnsafeLockError(f"{label} path is a symbolic link: {absolute}")
    if not stat.S_ISDIR(current_before.st_mode) or not stat.S_ISDIR(
        current_after.st_mode
    ):
        raise UnsafeLockError(f"{label} path is not a directory: {absolute}")
    if _metadata_tuple(opened_before) != _metadata_tuple(opened_after):
        raise UnsafeLockError(f"{label} descriptor metadata changed: {absolute}")
    if _metadata_tuple(current_before) != _metadata_tuple(current_after):
        raise UnsafeLockError(f"{label} path metadata changed: {absolute}")
    if (current_after.st_dev, current_after.st_ino) != (
        opened_after.st_dev,
        opened_after.st_ino,
    ):
        raise UnsafeLockError(f"{label} path changed: {absolute}")
    if current_after.st_uid != opened_after.st_uid or stat.S_IMODE(
        current_after.st_mode
    ) != stat.S_IMODE(opened_after.st_mode):
        raise UnsafeLockError(f"{label} path metadata changed: {absolute}")


def _lock_nonblocking(descriptor: int) -> None:
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        _fail("UI screen rendering is already running", LOCK_BUSY_EXIT)
    except OSError as error:
        _fail(f"cannot lock repository namespace: {error}", UNSAFE_LOCK_EXIT)


def _open_validated_directory(path: Path, label: str) -> int:
    try:
        descriptor = open_directory_no_symlinks(path)
        _validate_descriptor(descriptor, path, label)
    except UnsafeLockError as error:
        _fail(str(error), UNSAFE_LOCK_EXIT)
    return descriptor


def acquire_and_exec(repository: Path, command: list[str]) -> NoReturn:
    if not command:
        _fail("lock-protected renderer command is empty", 64)
    absolute = Path(os.path.abspath(os.fspath(repository)))
    parent = absolute.parent
    lock_descriptor = _open_validated_directory(parent, "repository parent")
    repository_descriptor = _open_validated_directory(absolute, "repository")
    _lock_nonblocking(lock_descriptor)
    try:
        _validate_descriptor(lock_descriptor, parent, "repository parent")
        _validate_descriptor(repository_descriptor, absolute, "repository")
    except UnsafeLockError as error:
        os.close(repository_descriptor)
        os.close(lock_descriptor)
        _fail(str(error), UNSAFE_LOCK_EXIT)
    os.set_inheritable(lock_descriptor, True)
    os.set_inheritable(repository_descriptor, True)
    environment = os.environ.copy()
    environment[LOCK_HELD_ENV] = "1"
    environment[LOCK_FD_ENV] = str(lock_descriptor)
    environment[REPOSITORY_FD_ENV] = str(repository_descriptor)
    try:
        os.execvpe(command[0], command, environment)
    except OSError as error:
        os.close(repository_descriptor)
        os.close(lock_descriptor)
        _fail(f"cannot invoke lock-protected renderer: {error}", 69)


def validate_inherited_lock(
    repository: Path, lock_descriptor: int, repository_descriptor: int
) -> None:
    absolute = Path(os.path.abspath(os.fspath(repository)))
    parent = absolute.parent
    try:
        _validate_descriptor(lock_descriptor, parent, "repository parent")
        _validate_descriptor(repository_descriptor, absolute, "repository")
    except UnsafeLockError as error:
        _fail(str(error), UNSAFE_LOCK_EXIT)
    _lock_nonblocking(lock_descriptor)
    try:
        _validate_descriptor(lock_descriptor, parent, "repository parent")
        _validate_descriptor(repository_descriptor, absolute, "repository")
    except UnsafeLockError as error:
        _fail(str(error), UNSAFE_LOCK_EXIT)


def main(arguments: list[str]) -> int:
    if len(arguments) >= 5 and arguments[1] == "acquire" and arguments[3] == "--":
        acquire_and_exec(Path(arguments[2]), arguments[4:])
    if len(arguments) == 5 and arguments[1] == "validate":
        try:
            lock_descriptor = int(arguments[3])
            repository_descriptor = int(arguments[4])
        except ValueError:
            _fail("repository lock descriptors must be integers", UNSAFE_LOCK_EXIT)
        validate_inherited_lock(
            Path(arguments[2]), lock_descriptor, repository_descriptor
        )
        return 0
    print(
        f"usage: {arguments[0]} acquire REPOSITORY -- COMMAND [ARG ...]\n"
        f"       {arguments[0]} validate REPOSITORY LOCK_FD REPOSITORY_FD",
        file=sys.stderr,
    )
    return 64


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

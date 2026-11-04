#!/usr/bin/env python3
from __future__ import annotations

import fcntl
import os
import signal
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from publish_screens import PublishError, publish_directory


REPOSITORY = Path(__file__).resolve().parents[2]
LOCK_HELPER = REPOSITORY / "tools" / "uisim" / "secure_render_lock.py"
RENDER_SCRIPT = REPOSITORY / "scripts" / "render-ui-screens.sh"


class SecureRepositoryLockTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.repository = self.root / "repository"
        self.repository.mkdir(mode=0o700)
        self.child_marker = self.root / "child-ran"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_helper(
        self, child_code: str, repository: Path | None = None
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(LOCK_HELPER),
                "acquire",
                str(repository or self.repository),
                "--",
                sys.executable,
                "-c",
                child_code,
            ],
            text=True,
            capture_output=True,
            timeout=20,
            check=False,
        )

    def test_second_renderer_fails_before_child_runs(self) -> None:
        descriptor = os.open(self.repository.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            completed = self.run_helper(
                f"from pathlib import Path; Path({str(self.child_marker)!r}).touch()"
            )
        finally:
            os.close(descriptor)
        self.assertEqual(completed.returncode, 75, completed.stdout + completed.stderr)
        self.assertIn("already running", completed.stderr)
        self.assertFalse(self.child_marker.exists())

    def test_symlinked_repository_path_is_rejected(self) -> None:
        alias = self.root / "repository-link"
        alias.symlink_to(self.repository, target_is_directory=True)
        completed = self.run_helper("raise AssertionError('child must not run')", alias)
        self.assertEqual(completed.returncode, 73, completed.stdout + completed.stderr)
        self.assertIn("symbolic link", completed.stderr)
        self.assertFalse(self.child_marker.exists())

    def test_group_or_world_writable_repository_is_rejected(self) -> None:
        self.repository.chmod(0o777)
        completed = self.run_helper("raise AssertionError('child must not run')")
        self.assertEqual(completed.returncode, 73, completed.stdout + completed.stderr)
        self.assertIn("permissions", completed.stderr)

    def test_child_inherits_repository_lock_and_descriptor(self) -> None:
        child_code = "\n".join(
            [
                "import fcntl, os",
                "from pathlib import Path",
                "lock_fd = int(os.environ['LIBRE_DESK_RENDER_LOCK_FD'])",
                "repository_fd = int(os.environ['LIBRE_DESK_RENDER_REPOSITORY_FD'])",
                "assert os.environ['LIBRE_DESK_RENDER_LOCK_HELD'] == '1'",
                "opened = os.fstat(repository_fd)",
                f"path = Path({str(self.repository)!r})",
                "current = path.stat(follow_symlinks=False)",
                "assert (opened.st_dev, opened.st_ino) == (current.st_dev, current.st_ino)",
                "contender = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)",
                "try:",
                "    try:",
                "        fcntl.flock(contender, fcntl.LOCK_EX | fcntl.LOCK_NB)",
                "    except BlockingIOError:",
                "        pass",
                "    else:",
                "        raise AssertionError('repository lock was not held')",
                "finally:",
                "    os.close(contender)",
                f"Path({str(self.child_marker)!r}).write_text('locked', encoding='utf-8')",
            ]
        )
        completed = self.run_helper(child_code)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        self.assertEqual(self.child_marker.read_text(encoding="utf-8"), "locked")

    def test_replaced_repository_path_cannot_create_a_second_lock_domain(self) -> None:
        moved = self.root / "repository-moved"
        second_marker = self.root / "second-child-ran"
        child_code = "\n".join(
            [
                "import os, subprocess, sys",
                "from pathlib import Path",
                f"repository = Path({str(self.repository)!r})",
                f"moved = Path({str(moved)!r})",
                "repository.rename(moved)",
                "repository.mkdir(mode=0o700)",
                "completed = subprocess.run([",
                f"    sys.executable, {str(LOCK_HELPER)!r}, 'acquire', str(repository), '--',",
                f"    sys.executable, '-c', \"from pathlib import Path; Path({str(second_marker)!r}).touch()\"",
                "], text=True, capture_output=True, check=False)",
                "assert completed.returncode == 75, completed.stdout + completed.stderr",
                f"assert not Path({str(second_marker)!r}).exists()",
            ]
        )
        completed = self.run_helper(child_code)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        self.assertFalse(second_marker.exists())

    def test_render_script_rejects_forged_marker_without_descriptor(self) -> None:
        environment = os.environ.copy()
        environment["LIBRE_DESK_RENDER_LOCK_HELD"] = "1"
        environment.pop("LIBRE_DESK_RENDER_LOCK_FD", None)
        environment.pop("LIBRE_DESK_RENDER_REPOSITORY_FD", None)
        completed = subprocess.run(
            [str(RENDER_SCRIPT)],
            cwd=REPOSITORY,
            env=environment,
            text=True,
            capture_output=True,
            timeout=20,
            check=False,
        )
        self.assertEqual(completed.returncode, 73, completed.stdout + completed.stderr)
        self.assertIn("descriptor is missing", completed.stderr)


class PublishTransactionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.docs = self.root / "docs"
        self.docs.mkdir(mode=0o700)
        self.target = self.docs / "screens"
        self.target.mkdir(mode=0o755)
        (self.target / "old.txt").write_text("old", encoding="utf-8")
        self.candidate = self.root / "candidate"
        self.candidate.mkdir(mode=0o700)
        (self.candidate / "new.txt").write_text("new", encoding="utf-8")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_signal_after_backup(
        self, *, fault_after_backup: bool
    ) -> subprocess.CompletedProcess[str]:
        program = "\n".join(
            [
                "import os, signal, sys",
                "from pathlib import Path",
                f"sys.path.insert(0, {str(REPOSITORY / 'tools' / 'uisim')!r})",
                "import publish_screens",
                "real_rename = publish_screens._rename_noreplace",
                "rename_count = 0",
                "def signal_after_first_rename(*args, **kwargs):",
                "    global rename_count",
                "    result = real_rename(*args, **kwargs)",
                "    rename_count += 1",
                "    if rename_count == 1:",
                "        os.kill(os.getpid(), signal.SIGTERM)",
                "    return result",
                "publish_screens._rename_noreplace = signal_after_first_rename",
                "publish_screens.publish_directory(",
                f"    Path({str(self.candidate)!r}), Path({str(self.target)!r}),",
                f"    fault_after_backup={fault_after_backup!r},",
                ")",
            ]
        )
        return subprocess.run(
            [sys.executable, "-c", program],
            text=True,
            capture_output=True,
            timeout=20,
            check=False,
        )

    def test_failed_install_restores_only_this_transactions_backup(self) -> None:
        stale = self.docs / ".screens.backup.12345"
        stale.mkdir()
        (stale / "unrelated.txt").write_text("unrelated", encoding="utf-8")
        with mock.patch("publish_screens.os.unlink", wraps=os.unlink) as unlink:
            with self.assertRaises(PublishError):
                publish_directory(self.candidate, self.target, fault_after_backup=True)
        unlink.assert_not_called()
        self.assertEqual((self.target / "old.txt").read_text(encoding="utf-8"), "old")
        self.assertEqual((stale / "unrelated.txt").read_text(encoding="utf-8"), "unrelated")
        self.assertTrue(self.candidate.is_dir())
        self.assertEqual(list(self.docs.glob(".screens-publish.*")), [])
        quarantines = list(self.docs.glob(".screens-quarantine.*"))
        self.assertEqual(len(quarantines), 1)
        retained_candidates = list(quarantines[0].rglob("new.txt"))
        self.assertEqual(len(retained_candidates), 1)
        self.assertEqual(retained_candidates[0].read_text(encoding="utf-8"), "new")

    def test_sigterm_after_backup_cannot_interrupt_commit_transition(self) -> None:
        completed = self.run_signal_after_backup(fault_after_backup=False)
        self.assertEqual(completed.returncode, -signal.SIGTERM)
        self.assertEqual((self.target / "new.txt").read_text(encoding="utf-8"), "new")
        self.assertFalse((self.target / "old.txt").exists())

    def test_sigterm_after_backup_cannot_interrupt_rollback_transition(self) -> None:
        completed = self.run_signal_after_backup(fault_after_backup=True)
        self.assertEqual(completed.returncode, -signal.SIGTERM)
        self.assertEqual((self.target / "old.txt").read_text(encoding="utf-8"), "old")
        self.assertFalse((self.target / "new.txt").exists())

    def test_successful_install_copies_files_and_sets_intentional_modes(self) -> None:
        publish_directory(self.candidate, self.target)
        self.assertTrue(self.candidate.is_dir())
        self.assertFalse((self.target / "old.txt").exists())
        self.assertEqual((self.target / "new.txt").read_text(encoding="utf-8"), "new")
        self.assertEqual(stat.S_IMODE(self.target.stat().st_mode), 0o755)
        self.assertEqual(stat.S_IMODE((self.target / "new.txt").stat().st_mode), 0o644)
        self.assertEqual((self.target / "new.txt").stat().st_nlink, 1)
        self.assertEqual(list(self.docs.glob(".screens-publish.*")), [])
        tombstones = list(self.docs.glob(".screens-quarantine.*"))
        self.assertEqual(len(tombstones), 1)
        retained_files = [path for path in tombstones[0].rglob("*") if path.is_file()]
        self.assertEqual(len(retained_files), 1)
        self.assertEqual(retained_files[0].name, "old.txt")
        self.assertEqual(retained_files[0].read_text(encoding="utf-8"), "old")

    def test_candidate_on_another_filesystem_is_copied_safely(self) -> None:
        shared_memory = Path("/dev/shm")
        if not shared_memory.is_dir():
            self.skipTest("/dev/shm is unavailable")
        if shared_memory.stat().st_dev == self.root.stat().st_dev:
            self.skipTest("/dev/shm is not a different filesystem")
        with tempfile.TemporaryDirectory(dir=shared_memory) as directory:
            candidate = Path(directory)
            candidate.chmod(0o700)
            (candidate / "cross.txt").write_text("cross-filesystem", encoding="utf-8")
            publish_directory(candidate, self.target)
        self.assertEqual(
            (self.target / "cross.txt").read_text(encoding="utf-8"),
            "cross-filesystem",
        )
        self.assertEqual(stat.S_IMODE((self.target / "cross.txt").stat().st_mode), 0o644)

    def test_non_regular_candidate_entry_is_rejected(self) -> None:
        (self.candidate / "new.txt").unlink()
        os.mkfifo(self.candidate / "named-pipe")
        with self.assertRaisesRegex(PublishError, "non-regular"):
            publish_directory(self.candidate, self.target)
        self.assertEqual((self.target / "old.txt").read_text(encoding="utf-8"), "old")

    def test_unowned_candidate_directory_is_rejected(self) -> None:
        with mock.patch("publish_screens.os.geteuid", return_value=os.geteuid() + 1):
            with self.assertRaisesRegex(PublishError, "not owned"):
                publish_directory(self.candidate, self.target)
        self.assertEqual((self.target / "old.txt").read_text(encoding="utf-8"), "old")

    def test_hardlinked_candidate_file_is_rejected_without_chmod(self) -> None:
        external = self.root / "external.txt"
        external.write_text("external", encoding="utf-8")
        external.chmod(0o600)
        (self.candidate / "new.txt").unlink()
        os.link(external, self.candidate / "new.txt")
        with self.assertRaisesRegex(PublishError, "hard link"):
            publish_directory(self.candidate, self.target)
        self.assertEqual(stat.S_IMODE(external.stat().st_mode), 0o600)
        self.assertEqual((self.target / "old.txt").read_text(encoding="utf-8"), "old")

    def test_symlinked_candidate_entry_is_rejected(self) -> None:
        (self.candidate / "new.txt").unlink()
        (self.candidate / "new.txt").symlink_to(self.root / "external.txt")
        with self.assertRaisesRegex(PublishError, "symbolic link"):
            publish_directory(self.candidate, self.target)
        self.assertEqual((self.target / "old.txt").read_text(encoding="utf-8"), "old")

    def test_symlinked_candidate_ancestor_is_rejected(self) -> None:
        alias = self.root / "candidate-link"
        alias.symlink_to(self.candidate, target_is_directory=True)
        with self.assertRaisesRegex(PublishError, "symbolic link"):
            publish_directory(alias, self.target)
        self.assertEqual((self.target / "old.txt").read_text(encoding="utf-8"), "old")

    def test_symlink_target_is_rejected(self) -> None:
        external = self.root / "external-target"
        external.mkdir()
        for child in self.target.iterdir():
            child.unlink()
        self.target.rmdir()
        self.target.symlink_to(external, target_is_directory=True)
        with self.assertRaisesRegex(PublishError, "symbolic link"):
            publish_directory(self.candidate, self.target)

    def test_hardlinked_existing_target_file_is_rejected(self) -> None:
        external = self.root / "external-old.txt"
        external.write_text("external-old", encoding="utf-8")
        (self.target / "old.txt").unlink()
        os.link(external, self.target / "old.txt")
        with self.assertRaisesRegex(PublishError, "hard link"):
            publish_directory(self.candidate, self.target)
        self.assertEqual(external.read_text(encoding="utf-8"), "external-old")
        self.assertEqual(external.stat().st_nlink, 2)

    def test_symlinked_target_parent_is_rejected(self) -> None:
        alias = self.root / "docs-link"
        alias.symlink_to(self.docs, target_is_directory=True)
        with self.assertRaisesRegex(PublishError, "symbolic link"):
            publish_directory(self.candidate, alias / "screens")
        self.assertEqual((self.target / "old.txt").read_text(encoding="utf-8"), "old")

    def test_transaction_quarantine_failure_keeps_the_valid_new_target(self) -> None:
        with mock.patch(
            "publish_screens._quarantine_transaction",
            side_effect=OSError("injected cleanup failure"),
        ):
            publish_directory(self.candidate, self.target)
        self.assertEqual((self.target / "new.txt").read_text(encoding="utf-8"), "new")
        self.assertFalse((self.target / "old.txt").exists())
        self.assertEqual(len(list(self.docs.glob(".screens-publish.*"))), 1)

    def test_post_install_path_swap_never_deletes_the_unrelated_replacement(self) -> None:
        victim = self.docs / "victim"
        victim.mkdir(mode=0o755)
        (victim / "victim.txt").write_text("do-not-delete", encoding="utf-8")
        displaced = self.docs / "displaced-new"
        real_validate = __import__("publish_screens")._validate_flat_directory_at
        injected = False

        def inject_swap(parent_descriptor: int, name: str, label: str) -> None:
            nonlocal injected
            if label == "installed publication target" and not injected:
                injected = True
                os.rename(
                    name,
                    displaced.name,
                    src_dir_fd=parent_descriptor,
                    dst_dir_fd=parent_descriptor,
                )
                os.rename(
                    victim.name,
                    name,
                    src_dir_fd=parent_descriptor,
                    dst_dir_fd=parent_descriptor,
                )
                raise PublishError("injected post-install path swap")
            real_validate(parent_descriptor, name, label)

        with mock.patch(
            "publish_screens._validate_flat_directory_at", side_effect=inject_swap
        ):
            with self.assertRaises(PublishError):
                publish_directory(self.candidate, self.target)

        self.assertEqual(
            (self.target / "victim.txt").read_text(encoding="utf-8"),
            "do-not-delete",
        )
        self.assertEqual((displaced / "new.txt").read_text(encoding="utf-8"), "new")
        transactions = list(self.docs.glob(".screens-publish.*"))
        self.assertEqual(len(transactions), 1)
        self.assertEqual(
            (transactions[0] / "backup" / "old.txt").read_text(encoding="utf-8"),
            "old",
        )

    def test_post_commit_path_swap_preserves_the_old_backup(self) -> None:
        victim = self.docs / "victim"
        victim.mkdir(mode=0o755)
        (victim / "victim.txt").write_text("do-not-delete", encoding="utf-8")
        displaced = self.docs / "displaced-new"
        module = __import__("publish_screens")
        real_validate = module._validate_named_directory_descriptor_at
        injected = False

        def inject_swap(
            parent_descriptor: int,
            name: str,
            descriptor: int,
            label: str,
        ) -> None:
            nonlocal injected
            if label == "committed publication target" and not injected:
                injected = True
                os.rename(
                    name,
                    displaced.name,
                    src_dir_fd=parent_descriptor,
                    dst_dir_fd=parent_descriptor,
                )
                os.rename(
                    victim.name,
                    name,
                    src_dir_fd=parent_descriptor,
                    dst_dir_fd=parent_descriptor,
                )
                raise PublishError("injected post-commit path swap")
            real_validate(parent_descriptor, name, descriptor, label)

        with mock.patch(
            "publish_screens._validate_named_directory_descriptor_at",
            side_effect=inject_swap,
        ):
            with self.assertRaises(PublishError):
                publish_directory(self.candidate, self.target)

        self.assertEqual(
            (self.target / "victim.txt").read_text(encoding="utf-8"),
            "do-not-delete",
        )
        self.assertEqual((displaced / "new.txt").read_text(encoding="utf-8"), "new")
        transactions = list(self.docs.glob(".screens-publish.*"))
        self.assertEqual(len(transactions), 1)
        self.assertEqual(
            (transactions[0] / "backup" / "old.txt").read_text(encoding="utf-8"),
            "old",
        )


    def test_install_noreplace_preserves_an_injected_empty_target(self) -> None:
        module = __import__("publish_screens")
        real_rename = module._rename_noreplace
        injected_inode: int | None = None

        def inject_target(
            source_parent: int,
            source_name: str,
            destination_parent: int,
            destination_name: str,
        ) -> None:
            nonlocal injected_inode
            if source_name == "new" and destination_name == self.target.name:
                os.mkdir(destination_name, 0o755, dir_fd=destination_parent)
                injected_inode = os.stat(
                    destination_name,
                    dir_fd=destination_parent,
                    follow_symlinks=False,
                ).st_ino
            real_rename(
                source_parent,
                source_name,
                destination_parent,
                destination_name,
            )

        with mock.patch(
            "publish_screens._rename_noreplace", side_effect=inject_target
        ):
            with self.assertRaises(PublishError):
                publish_directory(self.candidate, self.target)

        self.assertIsNotNone(injected_inode)
        self.assertEqual(self.target.stat().st_ino, injected_inode)
        self.assertEqual(list(self.target.iterdir()), [])
        transactions = list(self.docs.glob(".screens-publish.*"))
        self.assertEqual(len(transactions), 1)
        self.assertEqual(
            (transactions[0] / "backup" / "old.txt").read_text(encoding="utf-8"),
            "old",
        )

    def test_restore_noreplace_preserves_an_injected_empty_target(self) -> None:
        module = __import__("publish_screens")
        real_rename = module._rename_noreplace
        injected_inode: int | None = None

        def inject_target(
            source_parent: int,
            source_name: str,
            destination_parent: int,
            destination_name: str,
        ) -> None:
            nonlocal injected_inode
            if source_name == "backup" and destination_name == self.target.name:
                os.mkdir(destination_name, 0o755, dir_fd=destination_parent)
                injected_inode = os.stat(
                    destination_name,
                    dir_fd=destination_parent,
                    follow_symlinks=False,
                ).st_ino
            real_rename(
                source_parent,
                source_name,
                destination_parent,
                destination_name,
            )

        with mock.patch(
            "publish_screens._rename_noreplace", side_effect=inject_target
        ):
            with self.assertRaises(PublishError):
                publish_directory(
                    self.candidate,
                    self.target,
                    fault_after_backup=True,
                )

        self.assertIsNotNone(injected_inode)
        self.assertEqual(self.target.stat().st_ino, injected_inode)
        self.assertEqual(list(self.target.iterdir()), [])
        transactions = list(self.docs.glob(".screens-publish.*"))
        self.assertEqual(len(transactions), 1)
        self.assertEqual(
            (transactions[0] / "backup" / "old.txt").read_text(encoding="utf-8"),
            "old",
        )

    def test_swap_at_transaction_quarantine_entry_retains_the_backup(self) -> None:
        victim = self.docs / "victim"
        victim.mkdir(mode=0o755)
        (victim / "victim.txt").write_text("do-not-delete", encoding="utf-8")
        displaced = self.docs / "displaced-new"
        module = __import__("publish_screens")
        real_quarantine = module._quarantine_transaction
        injected = False

        def inject_swap(*args: object, **kwargs: object) -> None:
            nonlocal injected
            if not injected:
                injected = True
                guard = kwargs["guard"]
                assert isinstance(guard, tuple)
                parent_descriptor, target_name, _, _ = guard
                assert isinstance(parent_descriptor, int)
                assert isinstance(target_name, str)
                os.rename(
                    target_name,
                    displaced.name,
                    src_dir_fd=parent_descriptor,
                    dst_dir_fd=parent_descriptor,
                )
                os.rename(
                    victim.name,
                    target_name,
                    src_dir_fd=parent_descriptor,
                    dst_dir_fd=parent_descriptor,
                )
            real_quarantine(*args, **kwargs)

        with mock.patch(
            "publish_screens._quarantine_transaction", side_effect=inject_swap
        ):
            with self.assertRaises(PublishError):
                publish_directory(self.candidate, self.target)

        self.assertEqual(
            (self.target / "victim.txt").read_text(encoding="utf-8"),
            "do-not-delete",
        )
        self.assertEqual((displaced / "new.txt").read_text(encoding="utf-8"), "new")
        transactions = list(self.docs.glob(".screens-publish.*"))
        self.assertEqual(len(transactions), 1)
        self.assertEqual(
            (transactions[0] / "backup" / "old.txt").read_text(encoding="utf-8"),
            "old",
        )

    def test_successful_publish_deletes_no_files(self) -> None:
        with mock.patch(
            "publish_screens.os.unlink",
            side_effect=AssertionError("successful publication must retain prior files"),
        ):
            publish_directory(self.candidate, self.target)
        self.assertEqual((self.target / "new.txt").read_text(encoding="utf-8"), "new")
        tombstones = list(self.docs.glob(".screens-quarantine.*"))
        self.assertEqual(len(tombstones), 1)
        self.assertEqual(
            next(tombstones[0].rglob("old.txt")).read_text(encoding="utf-8"),
            "old",
        )

    def test_successful_publish_uses_no_name_based_rmdir(self) -> None:
        with mock.patch(
            "publish_screens.os.rmdir",
            side_effect=AssertionError("name-based rmdir must not be used"),
        ):
            publish_directory(self.candidate, self.target)
        self.assertEqual((self.target / "new.txt").read_text(encoding="utf-8"), "new")


if __name__ == "__main__":
    unittest.main()

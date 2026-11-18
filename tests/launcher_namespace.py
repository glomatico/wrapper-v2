"""Exercise the production launcher in a disposable outer PID and mount namespace."""
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time
import unittest

PROC = Path("/proc")
LAUNCHER = None


def children(pid):
    return {int(n) for n in (PROC / f"{pid}/task/{pid}/children").read_text().split()}


def eventually(check):
    deadline = time.monotonic() + 5
    while not check():
        if time.monotonic() >= deadline:
            raise AssertionError("process tree did not reach expected state")
        time.sleep(0.01)


class LauncherTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name)
        worker = cls.root / "system/bin/apple-music-worker"
        worker.parent.mkdir(parents=True)
        subprocess.run(["cc", "-static", "-Wall", "-Werror",
                        Path(__file__).with_name("launcher_probe.c"), "-o", worker], check=True)
        if int((PROC / "sys/kernel/pid_max").read_text()) > 70020:
            (PROC / "sys/kernel/ns_last_pid").write_text("70000")
            cls.high_pid = True
        else:
            cls.high_pid = False

    def start(self):
        proc = subprocess.Popen([LAUNCHER, self.root], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE)
        self.addCleanup(self.stop, proc)
        if self.high_pid:
            self.assertGreater(proc.pid, 65535)
        self.assertTrue(select.select([proc.stdout], [], [], 5)[0], "startup timed out")
        self.assertEqual(proc.stdout.readline(), b"2 1 2\n")
        init, = children(proc.pid)
        # The probe leaves one sleeping orphan and one short-lived orphan.
        # PID 1 must reap the latter while the worker is still running.
        def reaped():
            pids = children(init)
            try:
                return (len(pids) == 2 and
                        all((PROC / f"{pid}/stat").read_text().split()[2] != "Z" for pid in pids))
            except FileNotFoundError:
                return False  # An orphan was reaped between procfs reads.
        eventually(reaped)
        pids = children(init)
        worker, = [pid for pid in pids if (PROC / f"{pid}/status").read_text()
                   .split("NSpid:")[1].splitlines()[0].split()[-1] == "2"]
        self.assertEqual(sorted(os.listdir(PROC / f"{worker}/fd")), ["0", "1", "2"])
        self.assertNotEqual(os.stat(PROC / f"{worker}/ns/pid").st_ino,
                            os.stat(PROC / "self/ns/pid").st_ino)
        return proc, init, pids

    @staticmethod
    def stop(proc):
        proc.stdout.close()
        proc.stdin.close()
        if proc.poll() is None:
            proc.kill()
        proc.wait(timeout=5)

    def gone(self, pids):
        self.assertTrue(all(not (PROC / str(pid)).exists() for pid in pids))

    def test_repeated_exit_and_signal_status(self):
        for command, status in [(b"e", 37), (b"s", 128 + signal.SIGTERM)] * 4:
            proc, init, pids = self.start()
            proc.stdin.write(command)
            proc.stdin.flush()
            self.assertEqual(proc.wait(timeout=5), status)
            self.gone({init} | pids)

    def test_supervisor_closes_response_pipe(self):
        proc, init, pids = self.start()
        for pid in pids:
            os.kill(pid, signal.SIGSTOP)
        proc.stdout.close()  # Also happens on supervisor death, even before main().
        self.assertEqual(proc.wait(timeout=5), 0)
        self.gone({init} | pids)

    def test_abrupt_launcher_death(self):
        for sig in [signal.SIGTERM, signal.SIGKILL]:
            proc, init, pids = self.start()
            proc.send_signal(sig)
            self.assertEqual(proc.wait(timeout=5), -sig)
            # The test runner is the outer init, like Docker's init: true.
            eventually(lambda: os.waitpid(init, os.WNOHANG)[0] == init)
            self.gone({init} | pids)

    def test_supervisor_killed(self):
        reader, writer = os.pipe()
        supervisor = os.fork()
        if supervisor == 0:
            os.close(reader)
            try:
                proc, init, pids = self.start()
                os.write(writer, " ".join(map(str, [proc.pid, init, *pids])).encode())
                os.close(writer)
                while True:
                    signal.pause()
            finally:
                os._exit(1)
        os.close(writer)
        try:
            with os.fdopen(reader, "rb") as ready:
                self.assertTrue(select.select([ready], [], [], 6)[0])
                launcher, *descendants = map(int, ready.read().split())
        finally:
            os.kill(supervisor, signal.SIGKILL)
            os.waitpid(supervisor, 0)
        eventually(lambda: os.waitpid(launcher, os.WNOHANG)[0] == launcher)
        self.gone(descendants)

    def test_exec_failure(self):
        with tempfile.TemporaryDirectory() as empty_root:
            proc = subprocess.run([LAUNCHER, empty_root], capture_output=True, timeout=5)
        self.assertEqual(proc.returncode, 70)
        self.assertEqual(proc.stdout, b"")


if __name__ == "__main__":
    if sys.argv[1] != "--inside":
        os.execvp("unshare", ["unshare", "--mount", "--pid", "--fork", "--kill-child",
                              "--mount-proc", sys.executable, __file__, "--inside",
                              str(os.stat("/proc/self/ns/pid").st_ino), *sys.argv[1:]])
    del sys.argv[1]
    outer = int(sys.argv.pop(1))
    if (os.getpid() != 1 or os.readlink("/proc/self") != "1" or
            os.stat("/proc/self/ns/pid").st_ino == outer):
        raise RuntimeError("refusing to change PID allocation outside a fresh test namespace")
    LAUNCHER = Path(sys.argv.pop(1)).resolve(strict=True)
    unittest.main()

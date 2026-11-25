#!/usr/bin/env python3
"""Test wrapper of cmake/test_cap.cmake (kenshi-k16.15.55): runs the test command and appends "<test>\t<cpu_s>\t<ctest pid>" to the log file.
The ctest pid (our parent) is also exported as KSIM_CTEST_PID, so the check judges only the tests of its own ctest run.
CPU = user+sys of the test and every descendant it waited for (wait4 rusage), so it does not depend on the load of the box.
Usage: test_cpu_wrap.py <test> <log> <command> [args...]"""
import os, signal, subprocess, sys

name, log, cmd = sys.argv[1], sys.argv[2], sys.argv[3:]
if not cmd:
	sys.exit(2)
ppid = str(os.getppid())
child = subprocess.Popen(cmd, env=dict(os.environ, KSIM_CTEST_PID=ppid))
for sig in (signal.SIGTERM, signal.SIGINT):
	signal.signal(sig, lambda s, f: child.send_signal(s))
while True:
	try:
		_, status, ru = os.wait4(child.pid, 0)
		break
	except InterruptedError:
		continue
try:
	fd = os.open(log, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o644)
	os.write(fd, ("%s\t%.2f\t%s\n" % (name, ru.ru_utime + ru.ru_stime, ppid)).encode())
	os.close(fd)
except OSError:
	pass
rc = os.waitstatus_to_exitcode(status)
if rc < 0:	# died of a signal: die of it too, so ctest reports SEGFAULT / Subprocess aborted
	signal.signal(-rc, signal.SIG_DFL)
	os.kill(os.getpid(), -rc)
sys.exit(rc)

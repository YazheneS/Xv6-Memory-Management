import pexpect
import sys

cmd = ("qemu-system-riscv64 -machine virt -bios none -kernel kernel/kernel "
       "-m 128M -smp 3 -nographic -global virtio-mmio.force-legacy=false "
       "-drive file=fs.img,if=none,format=raw,id=x0 "
       "-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0")

tests = sys.argv[1].split(",") if len(sys.argv) > 1 else []

child = pexpect.spawn(cmd, cwd="/home/claude/xv6-riscv", timeout=30, encoding='utf-8')
child.logfile = None

results = {}
try:
    child.expect("init: starting sh")
    child.expect("\\$ ")
    for t in tests:
        child.sendline(f"usertests {t}")
        idx = child.expect(["\\$ ", pexpect.TIMEOUT, pexpect.EOF], timeout=60)
        out = child.before
        leaked = "FAILED -- lost" in out
        passed = "ALL TESTS PASSED" in out
        results[t] = "LEAK" if leaked else ("ok" if passed else "??")
        print(t, results[t], flush=True)
        if idx != 0:
            print("  (did not return to prompt cleanly)")
            break
except pexpect.TIMEOUT:
    print("TIMEOUT")
except pexpect.EOF:
    print("EOF / crash")
finally:
    child.close(force=True)

print("\n--- summary ---")
for t, r in results.items():
    if r != "ok":
        print(t, r)

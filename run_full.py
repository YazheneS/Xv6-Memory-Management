import pexpect
import sys

cmd = ("qemu-system-riscv64 -machine virt -bios none -kernel kernel/kernel "
       "-m 128M -smp 3 -nographic -global virtio-mmio.force-legacy=false "
       "-drive file=fs.img,if=none,format=raw,id=x0 "
       "-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0")

child = pexpect.spawn(cmd, cwd=".", timeout=30, encoding='utf-8')
child.logfile = None
try:
    child.expect("init: starting sh")
    child.expect("\\$ ")
    child.sendline("usertests -q")
    child.expect(["\\$ ", pexpect.TIMEOUT, pexpect.EOF], timeout=180)
    out = child.before
    # print only the tail + the pass/fail verdict line
    lines = out.splitlines()
    for l in lines:
        if "FAILED" in l or "PASSED" in l or "lost" in l or "DBG" in l:
            print(l)
except Exception as e:
    print("ERROR:", e)
finally:
    child.close(force=True)

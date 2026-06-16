print("exec _boot.py")
import ameba, os, sys
# Try to mount the filesystem, and format the flash if it doesn't exist.
bdev = ameba.Flash()

# try: 
#     vfs = os.VfsFat(bdev)
#     os.mount(vfs, "/")
# except:
#     print("[MP]: Creating VFS over FLASH..")
#     os.VfsFat.mkfs(bdev)
#     vfs = os.VfsFat(bdev)
#     print("[MP]: Created VFS over FLASH")
#     os.mount(vfs, "/")

try: 
    vfs = os.VfsLfs2(bdev)
    os.mount(vfs, "/")
except:
    print("[MP]: Creating VFS over FLASH..")
    os.VfsLfs2.mkfs(bdev)
    vfs = os.VfsLfs2(bdev)
    print("[MP]: Created VFS over FLASH")
    os.mount(vfs, "/")
sys.path.append("/lib")

#del bdev, vfs



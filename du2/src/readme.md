# Filesystem using inode design

## Memory interface
- size_t hdd_size() - Velkost harddisku v B
- hdd_read(size_t sector, void* buffer)
- hdd_write(size_t sector, void* buffer)

## User restrictions
- povolené znaky v názvoch súborov a adresárov: `a-zA-Z0-9.-_`; názvy sú citlivé na veľkosť písmen (ofc)
- maximálna veľkosť súboru 2^30 B (1 GiB)
- maximálna veľkosť disku 2^31 B (2 GiB)
- maximálny počet položiek v adresári 2^16 // tolko inodes ani nebudeme mat - mozno prave tolko inodes
- max. 16 otvorených súborov naraz
- súbor bude otvorený najviac raz (pri otváraní nezisťujete, či už súbor nebol otvorený)
- otvorené súbory sa nebudú mazať ani premenúvať (pri mazaní neskúmate, či je súbor otvorený alebo nie)
- v prípade hardlinkov je otvorený naraz najviac jeden z nich
- hardlinky sa vytvárajú len pre obyčajné súbory
- MAX_FILENAME 12 - Maximalna dlzka nazvu suboru v znakoch
- MAX_PATH 64 - Maximalna dlzka cesty v znakoch
- SECTOR_SIZE 128 - Velkost sektora na disku v bajtoch

### Distilled
- names don't contain "/"
- disk 2GB, file 1GB
- max open 16 at a time
- rm/mv never called on open files
- 1 concurrent hardlink open
- hardlinks only for files
- 2^16*(12+2) // max files in dir *( filename limit (ASCII)+ inode index ) - avsak max suborov v systeme bude pravdepodobne daleko menej ako 2^16

## Design
- inodes with extents

### Overview
- 0th sector = magic number, bitmap size, inode count - rest is data
- 1st - nth sector = bitmap - one sector covers 128*8 = 1024 sectors, n = hdd_size()/1024
- n+1st - n+2^12 = inodes, 2^12 = 4096 inodes NOPE! - we need to be able to allocate 1/4th of the sector size in files
- rest - data sectors

### Superblock
- 

### Bitmap
- 

### Inode
- METADATA, 12B
    - size // 4B
    - link_count // 4B
    - type // 4B
- DATA, 116B 
    - 24 direct // 96B
    - 5 indirect // 20B
        - 1 single
        - 1 double
        - 1 triple
        - 1 quadruple
        - 1 quintuple

### Constraints
- below some tests that the FS is supposed to clear
- first test: the minimum number of allocable empty files (?=inodes) == 1/4 of sector count
- second test: the minimum number of allocable small files (~10B) == 1/6 of sector count
- third test: the system should be able to alloc 1/3 of the disk space to large files (~48 sectors per file)

# efibootgen
A simple, cross platform, tool to generate (U)EFI bootable disk images with.

This tool creates a FAT formatted, GPT partitioned, disk image that can be used to boot UEFI kernels with (for example)</br>

## usage
To create an image from a directory:

```efibootgen -d <SOURCE DIRECTORY> -o <OUTPUT DISK IMAGE FILE>```

To create a standard EFI\BOOT\BOOTX64.EFI layout
```efibootgen -b <PATH TO BOOTX64.EFI> -o <OUTPUT DISK IMAGE FILE>```

### other options
-v, --verbose           output more information about the build process</br>
-c, --case              preserve case of filenames. Default converts to UPPER</br>
-l, --label             volume label of image</br>
-h, --help              about this application</br>

## to build
The project is built with Visual Studio 2019 and requires C++ 17 standard support. 
The code itself is generic and should be straight forward to build and use with GCC or Clang.


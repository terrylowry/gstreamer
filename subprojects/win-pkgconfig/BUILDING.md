## Steps to build on Windows

- Install MSYS2
- Run MSYS2 MINGW32 command prompt
- Run these commands:

```
pacman -S msys/zip mingw32/mingw-w64-i686-meson
meson setup --optimization=s --strip \
        --wrap-mode=forcefallback \
        -Dpc_path='' \
        -Dsystem_include_path='' \
        -Dsystem_library_path='' \
        -Ddefine_prefix=disabled \
        _mingw32
meson install --destdir $PWD/install -C _mingw32
cp install/bin/pkg-config.exe .
zip pkg-config-0.29.2.1.zip pkg-config.exe
```

You may need to do some extra fiddling to install Meson. The tested version is
Meson 1.8.2.

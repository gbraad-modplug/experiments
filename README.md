# experiments

```
./configure --enable-interactive
make
sudo make install
sudo ldconfig /usr/local/lib
```

```
gcc interactive.c -o modplayer \
    $(pkg-config --cflags --libs sdl2 libopenmpt)
```

```
LD_LIBRARY_PATH=/usr/local/lib ./modplayer /mnt/c/Users/gbraad/Downloads/*.mod
```

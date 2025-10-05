# experiments

```
./configure --enable-interactive
make
sudo make install
sudo ldconfig /usr/local/lib
```

```
gcc modplayer_full.c -o modplayer \
    $(pkg-config --cflags --libs sdl2 libopenmpt)
```

```
LD_LIBRARY_PATH=/usr/local/lib ./modplayer /mnt/c/Users/gbraad/Downloads/*.mod
```

  - https://youtu.be/FEXjETcyd5s
  - https://youtu.be/pi2ZJRXj5uQ

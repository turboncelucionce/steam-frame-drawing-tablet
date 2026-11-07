# Use your pen tablet in Steam Frame's native desktop

Turns your pen tablet compatible with the Steam Frame, since it doesn't ship with the drivers by default

Currently doesn't support tablet buttons and touch, but pens buttons and sensitivity work (Tested with Wacom CTH-670 in Krita)

## How to install

Make sure you have a sudo password (if you don't, run `passwd` in Konsole).

1. Open Native Desktop, then **Konsole**
2. Run:

   ```bash
   git clone https://github.com/turboncelucionce/steam-frame-drawing-tablet.git ~/steamframe-pentablet
   bash ~/steamframe-pentablet/install.sh
   ```
3. Type your password when prompted
4. Restart native desktop
5. Open **OpenTabletDriver** from the app menu if it doesn't on its own, and set it to **Artist Mode** at the bottom
6. It should work

### If an update breaks something

Try running:

```bash
bash ~/steamframe-pentablet/kwin-otd-tablet/install.sh
```
## License

GPL-2.0-or-later. See [LICENSE](LICENSE).

## Engines from *Simulating an F1 V12, cross-plane and flat-plane V8s, unequal length headers and more*

This folder contains all the engines from [*Simulating an F1 V12, cross-plane and flat-plane V8s, unequal length headers and more*](https://youtu.be/NBBwva3NZ6o). Each file contains a node called `main` and it can be used in `main.mr` as follows.

```
import "engines/atg-video-2/<insert filename>"

main()
```

The following engines are included:
1. Subaru EJ25 with equal length headers
2. Subaru EJ25 with unequal length headers
3. Toyota 2JZ
4. 60 degree V6
5. Odd-fire 90 degree V6
6. Even-fire 90 degree V6
7. GM LS V8
8. Ferrari F136 V8
9. Radial-9
10. Toyota 1LR-GUE (Lexus LFA)
11. Merlin V12
12. Ferrari Tipo 044/1 (Formula 1 V12)

The [first six presets](presets-01-06.json) have canonical JSON files beside
their scripts. Each comes from its own `main()` selection. The two EJ25
header layouts and the odd-fire and even-fire V6 selections remain distinct.
Use the common data directory containing `assets/` and `es/` as the asset
root when loading these files.

The JSON derives from the model scripts under the project's MIT terms.
Impulse locators reference the existing sound library. Recording distribution
is governed by the corresponding asset rights.

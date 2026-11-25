# Survival Adrenaline

An SKSE plugin for Skyrim SE/AE Survival Mode. Adrenaline keeps you going in dungeons and fights, and then you crash.

- **In a dungeon or in combat,** hunger and fatigue build up at 15% of their normal speed by default.
- **The rest is held back, not forgiven.** When you're out of combat and out of any dungeon, everything you didn't gain lands at once. You get the message *"The adrenaline fades. Your hunger and weariness catch up with you."* along with Survival Mode's usual hunger and fatigue stage messages.
- **Fighting inside a dungeon** uses the stronger of the two slowdowns. The crash waits until you've left the dungeon too.
- **Dungeons** are interiors whose location is marked as a dungeon or clearable, or as a crypt, Dwemer ruin, Falmer hive, vampire or warlock lair, hagraven nest, dragon priest lair, bandit camp, Forsworn camp or animal den.

## Compatibility

- **Survival Mode Improved - SKSE:** recommended. The mod scales SMI's own rate multipliers (`SMI_HungerRateMult` and `SMI_ExhaustionRateMult`). It never touches the base rates, which SMI resets on every load.
- **CC Survival Mode without SMI:** also works. The mod then scales Survival Mode's base hunger and fatigue rates.
- **Survival Control Panel and other MCMs:**
  - **Settings are kept:** whatever rate a menu set is the rate you return to after the crash.
  - **Changes made mid-adrenaline count:** if you change a rate in a menu during a dungeon or fight, your new value is kept and becomes the normal rate.
- **Saving mid-dungeon is safe:** the held-back amount and your normal rates are kept in the SKSE co-save.
- **The held-back amount is measured, not guessed.** The mod watches how much hunger and fatigue actually rose while slowed. Your race, vampirism, being over-encumbered and Survival Mode's other factors all end up in the crash exactly as they would have applied.

Eating during a fight lowers hunger as usual, but doesn't cancel what's held back. The crash can't push a need past Survival Mode's maximum.

## Settings

`SKSE/Plugins/SurvivalAdrenaline.ini`:
- **Speed in dungeons and in combat:** hunger and fatigue are set separately, from 0.05 to 1.0. Lower means stronger adrenaline and a bigger crash.
- **Crash message:** on or off.
- **Check interval:** how often the mod checks whether you're in a dungeon or combat.

## Requirements

- SKSE64 for your game version
- Address Library for SKSE Plugins ("All in one (Anniversary Edition)" for AE)
- Survival Mode (Creation Club / Anniversary Edition), with or without Survival Mode Improved - SKSE

## Uninstalling

Leave dungeons and combat first and let the crash happen. This restores the normal rates. Uninstalling mid-dungeon leaves the rates slowed in that save.

## Log

`Documents/My Games/Skyrim Special Edition/SKSE/SurvivalAdrenaline.log` records each slowdown, the crash, and the size of the held-back amount.

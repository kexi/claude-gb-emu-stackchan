# Built-in ROM

`kantan-gb-play.gbc` is fetched from the user-selected
[KANTAN GB PLAY repository](https://github.com/GOROman/kantan-gb-play) by
`scripts/fetch-rom.sh`. The source commit and SHA-256 digest are fixed in
`rom.lock`; the downloaded ROM remains ignored by Git and PlatformIO embeds it
into the local CoreS3 firmware.

The upstream repository does not declare a software or ROM license as of the
pinned commit. Do not redistribute the downloaded ROM or resulting firmware
without permission from the rights holder.

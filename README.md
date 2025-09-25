# UBI on Zephyr

This repository introduce support of [Unsorted Block Images (UBI)](http://www.linux-mtd.infradead.org/doc/ubi.html) on **Zephyr RTOS**.

## Overview

UBI (Latin: "where?") stands for "Unsorted Block Images". It is a volume management system for raw flash devices which manages multiple logical volumes on a single physical flash device and spreads the I/O load (i.e, wear-leveling) across whole flash chip.

In a sense, UBI may be compared to the Logical Volume Manager (LVM). Whereas LVM maps logical sectors to physical sectors, UBI maps logical eraseblocks to physical eraseblocks. But besides the mapping, UBI implements global wear-leveling and transparent error handling.

An UBI volume is a set of consecutive logical eraseblocks (LEBs). Each logical eraseblock is dynamically mapped to a physical eraseblock (PEB). This mapping is managed by UBI and is hidden from users and higher-level software. UBI is the base mechanism which provides global wear-leveling, per-physical eraseblock erase counters, and the ability to transparently move data from more worn-out physical eraseblocks to less worn-out ones.

The UBI volume size is specified when a volume is created, but may later be changed (volumes are dynamically re-sizable).

### Main features

- UBI provides volumes which may be dynamically created, removed, or re-sized;
- UBI implements wear-leveling across the entire flash device (i.e., you might think you're continuously writing/erasing the same logical eraseblock of an UBI volume, but UBI will spread this to all physical eraseblocks of the flash chip);
- UBI transparently handles bad physical eraseblocks;
- UBI minimizes the chances of losing data by means of scrubbing.

### Resource Usage

| Metric              | Version 0.4.0   |
|---------------------|-----------------|
| Flash Usage         | 2802 B          |
| Static RAM Usage    | 0 B             |

**Dynamic RAM Usage**

| Object   | Usage       |
|----------|-------------|
| Bad PEB  | 12  B each  |
| PEB      | 16  B each  |
| Volume   | 48  B each  |
| Device   | 112 B each  |

## Documentation

- ➡️ [environment setup](doc/environment_setup.md)
- ➡️ [features candidates](doc/features_candidates.md)

## Contributing

Contributions are welcome! To contribute:

1. Fork the repository and create a new branch.
2. Implement your feature or bugfix.
3. Write tests if applicable.
4. Open a pull request.

Please ensure your code follows the existing style and structure of the project.

## License

This library is published as open-source software without any warranty of any kind. Use is permitted under the terms of the MIT license.

## Contact

email: kamkie1996@gmail.com

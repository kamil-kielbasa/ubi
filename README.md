# UBI for Zephyr

This repository demonstrates the use of **Unsorted Block Images (UBI)** with **Zephyr RTOS**, focused on a basic and functional implementation for flash-backed storage use cases.

## Introduction

**UBI (Unsorted Block Images)** is a volume management layer used in Linux for raw flash devices. It provides wear-leveling, bad block management, and support for multiple logical volumes on top of flash memory. UBI is typically used together with a file system such as **UBIFS** to manage persistent data storage on NAND flash.

This project provides a minimal, functional integration of UBI within the Zephyr environment. It covers initialization of UBI, mounting, and simple file operations to demonstrate UBI’s usage in embedded applications. While full UBIFS is not supported natively in Zephyr, this project focuses on preparing and working with UBI-compatible block-level storage.

## Overview

The goal of this project is to demonstrate how to:

- Set up a Zephyr-based workspace for UBI support
- Build and flash sample/test applications for the `b_u585i_iot02a` board
- Use Zephyr's file system and flash management features with UBI

## Documentation

- ➡️ [environment setup](doc/environment_setup.md)
- ➡️ [hardware tests](doc/hardware_tests.md)
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

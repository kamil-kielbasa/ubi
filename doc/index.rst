UBI on Zephyr
=============

**Unsorted Block Images (UBI)** is a volume management layer for raw flash
devices running `Zephyr RTOS <https://www.zephyrproject.org/>`_.
It provides wear-leveling, bad block management, and multiple logical volumes
on a single flash partition — similar to what LVM does for block devices.

This is a from-scratch implementation targeting resource-constrained embedded
systems. It requires approximately **2.8 KB of flash** and **zero static RAM**.

.. toctree::
   :maxdepth: 2
   :caption: Documentation

   introduction
   architecture
   getting_started
   configuration
   test_strategy
   api

.. toctree::
   :maxdepth: 1
   :caption: Project

   roadmap
   contributing
   changelog

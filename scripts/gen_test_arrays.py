import random

sizes = [5, 97, 271, 3907]

with open("random_arrays.h", "w") as f:
    f.write("#ifndef RANDOM_ARRAYS_H\n")
    f.write("#define RANDOM_ARRAYS_H\n\n")
    f.write("#include <stdint.h>\n\n")

    for size in sizes:
        arr_name = f"array_{size}"
        values = [f"0x{random.randint(0,255):02X}" for _ in range(size)]

        f.write(f"const uint8_t {arr_name}[] = {{\n")

        for i in range(0, size, 16):
            line = "    " + ", ".join(values[i:i+16]) + ","
            f.write(line + "\n")

        f.write("};\n\n")

    f.write("#endif // RANDOM_ARRAYS_H\n")

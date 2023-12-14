import argparse
from math import ceil


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("Q", type=int, help="The total size of the file system, in blocks.")
    args = parser.parse_args()
    Q = args.Q

    if Q < 77:
        print("Q cannot be less than 77.")
        return

    L = ceil((Q - 1) / 8305)
    N = ceil((512 * (Q - L - 1)) / 519)
    M = Q - 1 - L - N

    print(f"Q = {1 + M + N + L}")
    print(f"M = {M}")
    print(f"N = {N}")
    print(f"L = {L}")
    print(f"DIR_SIZE = ~{int(N / 4)}")


if __name__ == '__main__':
    main()

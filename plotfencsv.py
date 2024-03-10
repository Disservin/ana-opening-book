import argparse
import matplotlib.pyplot as plt
from collections import Counter


def format_large_number(number):
    suffixes = ["", "K", "M", "G", "T", "P"]
    for suffix in suffixes:
        if number < 1000:
            return f"{number:.0f}{suffix}"
        number /= 1000
    return f"{number:.0f}{suffixes[-1]}"


class csvdata:
    def __init__(self, prefix, drawRateMin, drawRateMax, outFile):
        self.prefix = prefix
        self.games = Counter()  # games played
        self.depth = Counter()  # depths (in plies)
        self.drawrate = Counter()  # draw rates (in percent)
        self.total_count = self.white_count = 0
        fens = set()
        filtered = []
        with open(prefix + ".csv") as f:
            for line in f:
                line = line.strip()
                if line.startswith("FEN"):
                    filtered.append(line)
                    continue
                if line:
                    fields = line.split(",")
                    fenfields = fields[0].split()
                    assert len(fenfields) >= 4, f"Incomplete FEN {fields[0]}"
                    fens.add(" ".join(fenfields[:4]))
                    if (
                        len(fenfields) >= 6
                        and fenfields[4].isdigit()
                        and fenfields[5].isdigit()
                    ):
                        move = int(fenfields[5])
                        ply = (
                            (move - 1) * 2
                            if fenfields[1] == "w"
                            else (move - 1) * 2 + 1
                        )
                        self.depth[ply] += 1

                if len(fields) >= 4:
                    W, D, L = int(fields[1]), int(fields[2]), int(fields[3])
                    G = W + D + L
                    self.games[G] += 1
                    if G:
                        dr = int(D / G * 100)
                        self.drawrate[dr] += 1
                        if drawRateMin is not None or drawRateMax is not None:
                            if (drawRateMin is None or dr >= drawRateMin) and (
                                drawRateMax is None or dr <= drawRateMax
                            ):
                                filtered.append(line)
                    self.total_count += G
                    self.white_count += G * (1 if fenfields[1] == "w" else 0)
        self.pos_count = len(fens)
        if drawRateMin is not None or drawRateMax is not None:
            with open(outFile, "w") as f:
                for line in filtered:
                    f.write(line + "\n")
            print(f"Saved {len(filtered)-1} filtered position stats to {outFile}.")
            outFile = outFile.replace(".csv", ".epd")
            with open(outFile, "w") as f:
                for line in filtered[1:]:
                    fen = line.split(",")[0]
                    f.write(fen + "\n")
            print(f"Saved {len(filtered)-1} filtered positions to {outFile}.")


def create_distribution_graph(csvs, plot="drawrate"):
    color, edgecolor = ["red", "blue"], ["yellow", "black"]
    if plot == "drawrate":
        countList = [c.drawrate for c in csvs]
    elif plot == "depth":
        countList = [c.depth for c in csvs]
    elif plot == "games":
        countList = [c.games for c in csvs]
    rangeMin, rangeMax = None, None
    for d in countList:
        mi, ma = min(d.keys()), max(d.keys())
        rangeMin = mi if rangeMin is None else min(mi, rangeMin)
        rangeMax = ma if rangeMax is None else max(ma, rangeMax)
    fig, ax = plt.subplots()
    fig.subplots_adjust(top=0.85)  # allow more space for legend above
    perBin = 1  # values per bin
    for Idx, d in enumerate(countList):
        white = csvs[Idx].white_count / csvs[Idx].total_count * 100
        g = format_large_number(csvs[Idx].total_count)
        gamesStr = f" {csvs[Idx].pos_count} positions (white:black = {white:.0f}:{100-white:.0f}), {g} games"
        ax.hist(
            d.keys(),
            weights=d.values(),
            range=(rangeMin, rangeMax),
            bins=(rangeMax - rangeMin) // perBin,
            alpha=0.5,
            color=color[Idx],
            edgecolor=edgecolor[Idx],
            label=csvs[Idx].prefix + gamesStr,
        )
    ax.legend(loc="upper center", bbox_to_anchor=(0.5, 1.12), ncol=1, fontsize=7)
    ax.ticklabel_format(axis="y", style="plain")
    plt.setp(ax.get_yticklabels(), fontsize=8)

    if plot == "drawrate":
        fig.suptitle("Drawrates (in %) across book exits, seen in games played.")
    elif plot == "depth":
        ax.set_yscale("log")
        fig.suptitle("Depths (in plies) of the book exits.")
    elif plot == "games":
        fig.suptitle("Games played per book exit.")
    plt.savefig(f"fencsv_{plot}.png", dpi=300)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Plot/filter data stored in results.csv created by ana-opening-book.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "filenames",
        nargs="*",
        help="File with FEN WDL statistics.",
        default=["results.csv"],
    )
    parser.add_argument(
        "--drawRateMin",
        type=float,
        help="Lower limit for draw rate filter if just one file is given.",
    )
    parser.add_argument(
        "--drawRateMax",
        type=float,
        help="Upper limit for draw rate filter if just one file is given.",
    )
    parser.add_argument(
        "--outFile",
        help="Filename for the filtered data.",
        default="filtered.csv",
    )
    args = parser.parse_args()
    if len(args.filenames) > 1 and (
        args.drawRateMin is not None or args.drawRateMax is not None
    ):
        print("Draw rate limits are only allowed for a single input file.")
        exit(1)

    csvs = []
    for f in args.filenames:
        prefix, _, _ = f.partition(".")
        csvs.append(csvdata(prefix, args.drawRateMin, args.drawRateMax, args.outFile))

    for plot in ["drawrate", "depth", "games"]:
        create_distribution_graph(csvs, plot=plot)

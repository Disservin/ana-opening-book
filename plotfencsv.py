import argparse
import numpy as np
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
    def __init__(self, prefix):
        self.prefix = prefix
        self.fenwdl = {}  # dict with mapping fenkey -> [fen, W, D, L]
        with open(prefix + ".csv") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("FEN"):
                    continue
                fields = line.split(",")
                assert len(fields) >= 4, f"Missing WDL data in {line}"
                fenfields = fields[0].split()
                assert len(fenfields) >= 4, f"Incomplete FEN {fields[0]}"
                fenkey = " ".join(fenfields[:4])  # ignore move counters
                fenfull = " ".join(fenfields[:6])
                if fenkey not in self.fenwdl:
                    self.fenwdl[fenkey] = [fenfull, 0, 0, 0]
                for i in [1, 2, 3]:
                    self.fenwdl[fenkey][i] += int(fields[i])

    def calculate_stats(self):
        self.games = Counter()  # frequencies of games played per book exit
        self.depth = Counter()  # book depths (in plies)
        self.drawrate = Counter()  # frequencies of draw rates (in percent)
        self.total_count = self.white_count = 0
        self.pos_count = len(self.fenwdl)
        for key in self.fenwdl:
            fenfields = self.fenwdl[key][0].split()
            if (
                len(fenfields) >= 6
                and fenfields[4].isdigit()
                and fenfields[5].isdigit()
            ):
                move = int(fenfields[5])
                ply = (move - 1) * 2 if fenfields[1] == "w" else (move - 1) * 2 + 1
                self.depth[ply] += 1

            W, D, L = self.fenwdl[key][1], self.fenwdl[key][2], self.fenwdl[key][3]
            G = W + D + L
            self.games[G] += 1
            if G:
                dr = int(D / G * 100)
                self.drawrate[dr] += 1
            self.total_count += G
            self.white_count += G * (1 if fenfields[1] == "w" else 0)

    def filter_exits(self, drawRateMin, drawRateMax, drawRateGames, outFile):
        count = 0
        epdFile = outFile.replace(".csv", ".epd")
        with open(outFile, "w") as f, open(epdFile, "w") as fepd:
            f.write("FEN, Wins, Draws, Losses\n")
            for key in self.fenwdl:
                W, D, L = self.fenwdl[key][1], self.fenwdl[key][2], self.fenwdl[key][3]
                G = W + D + L
                dr = int(D / G * 100) if G else 0
                if (
                    G < drawRateGames
                    or (drawRateMin is None or dr >= drawRateMin)
                    and (drawRateMax is None or dr <= drawRateMax)
                ):
                    f.write(f"{self.fenwdl[key][0]}, {W}, {D}, {L}\n")
                    fepd.write(f"{self.fenwdl[key][0]}\n")
                    count += 1
            print(
                f"Saved {count} filtered positions and stats to {epdFile} and {outFile}."
            )

    def create_games_per_exit_graph(self, bookFile):
        var = 0
        games = []
        with open(bookFile) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                fenfields = line.split()
                assert len(fenfields) >= 4, f"Incomplete FEN {line}"
                key = " ".join(fenfields[:4])  # ignore move counters
                G = 0
                if key in self.fenwdl:
                    for i in [1, 2, 3]:
                        G += self.fenwdl[key][i]
                games.append(G)
                var += G * G
        fig, ax = plt.subplots()
        mi, ma = min(games), max(games)
        fig.suptitle(f"Games played per book exit. (min: {mi}, max: {ma})")
        num_exits = len(games)
        mean = self.total_count / num_exits
        var = var / num_exits - mean**2
        ax.set_title(
            f"Games played in total: {self.total_count}. Per exit: mean={mean:.1f}, var={var:.1f}.",
            fontsize=7,
        )
        x_data = np.array(range(num_exits))
        y_data = np.array(games)
        ax.scatter(x_data, y_data, s=1, alpha=0.2, color="black", label="raw data")
        window_size = 5000
        rolling = np.convolve(y_data, np.ones(window_size) / window_size, mode="valid")
        x_data_rolling = x_data[(window_size - 1) // 2 : -(window_size - 1) // 2]
        ax.plot(
            x_data_rolling, rolling, color="red", linewidth=1, label="rolling average"
        )
        ax.set_xticks([0, len(games)])
        ax.xaxis.set_major_formatter(
            plt.FuncFormatter(lambda val, pos: "{:.0f}".format(val))
        )
        ax.set_xlabel("book exit (ordered as in .epd file)")
        ax.set_ylabel("# of games played")
        ax.legend(loc="upper left", fontsize=5, ncol=2)
        plt.savefig("games_per_exit.png", dpi=300)


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
    for Idx, d in enumerate(countList):
        infoStr = f" {csvs[Idx].pos_count} book exits"
        if plot != "depth":
            white = csvs[Idx].white_count / csvs[Idx].total_count * 100
            g = format_large_number(csvs[Idx].total_count)
            infoStr += f", {g} games (white:black = {white:.0f}:{100-white:.0f})"
        ax.hist(
            d.keys(),
            weights=d.values(),
            range=(rangeMin, rangeMax),
            bins=(rangeMax - rangeMin),
            alpha=0.5,
            color=color[Idx],
            edgecolor=edgecolor[Idx],
            label=csvs[Idx].prefix + infoStr,
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
        fig.suptitle("Frequencies of games played per book exit.")
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
        "--drawRateGames",
        type=int,
        help="Do not remove exits with fewer games.",
        default=10,
    )
    parser.add_argument(
        "--outFile",
        help="Filename for the filtered data.",
        default="filtered.csv",
    )
    parser.add_argument(
        "--bookFile",
        help="Filename for the (order of the) book exits, for extra plotting.",
    )
    args = parser.parse_args()
    if len(args.filenames) > 2:
        print("No more than two csv files allowed.")
        exit(1)

    if len(args.filenames) > 1 and (
        args.drawRateMin is not None
        or args.drawRateMax is not None
        or args.bookFile is not None
    ):
        print("Draw rate limits are only allowed for a single input file.")
        exit(1)

    csvs = []
    for f in args.filenames:
        prefix, _, _ = f.partition(".")
        csv = csvdata(prefix)
        csv.calculate_stats()
        if args.drawRateMin is not None or args.drawRateMax is not None:
            csv.filter_exits(
                args.drawRateMin, args.drawRateMax, args.drawRateGames, args.outFile
            )
        if args.bookFile:
            csv.create_games_per_exit_graph(args.bookFile)
        csvs.append(csv)

    for plot in ["drawrate", "depth", "games"]:
        create_distribution_graph(csvs, plot=plot)

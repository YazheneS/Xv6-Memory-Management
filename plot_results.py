import matplotlib.pyplot as plt

caps = [160, 160, 80, 40, 40]
children = [2, 4, 4, 4, 8]
file_faults = [175, 295, 196, 271, 339]
evictions = [143, 239, 135, 215, 222]
reloads = [113, 183, 99, 161, 159]

labels = [
    "160 frames, 2 children",
    "160 frames, 4 children",
    "80 frames, 4 children",
    "40 frames, 4 children",
    "40 frames, 8 children"
]

x = range(len(labels))

plt.plot(x, evictions, marker='o', label='Evictions')
plt.plot(x, reloads, marker='o', label='Reloads')

plt.xticks(x, labels, rotation=20, ha='right')
plt.xlabel("Experiment")
plt.ylabel("Count")
plt.title("Page Replacement: Evictions and Reloads")
plt.legend()
plt.tight_layout()

plt.savefig("page_replacement_graph.png", dpi=300)
plt.show()

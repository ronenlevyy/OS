
import pandas as pd
import matplotlib.pyplot as plt

data = pd.read_csv(r"/Users/ronenlevy/Desktop/OS/ex1/OS_Ex1/Resources/resulted2.csv")
data = data.to_numpy()

l1_size = 32*1024
l2_size = 256*1024
l3_size = 9*1024*1024

page_threshold = 2414600000

plt.plot(data[:, 0], data[:, 1], label="Random access")
plt.plot(data[:, 0], data[:, 2], label="Sequential access")
plt.xscale('log')
plt.yscale('log')
plt.axvline(x=l1_size, label="L1 (32 KiB)", c='r')
plt.axvline(x=l2_size, label="L2 (256 KiB)", c='g')
plt.axvline(x=l3_size, label="L3 (9 MiB)", c='brown')
plt.axvline(x=page_threshold  , label="page threshold (2.25 GiB)", c='blue')
plt.legend()
plt.title("Latency as a function of array size")
plt.ylabel("Latency (ns log scale)")
plt.xlabel("Bytes allocated (log scale)")
plt.show()

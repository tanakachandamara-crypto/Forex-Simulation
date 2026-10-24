\## **Prerequisites \& Installation**

\- GCC compiler with OpenMP support

\- libcurl development headers



\### **Configuration**

Set your environment variables before executing the binary:

```bash

export FOREX\_API\_KEY="your\_api\_key\_here"

export FRED\_API\_KEY="your\_api\_key\_here"





**Build and Run**

gcc -O3 main.c -fopenmp -lcurl -lm -o forex\_sim

./forex\_sim






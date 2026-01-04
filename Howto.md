g++ -std=c++17 main.cpp ConfigManager.cpp ResourceManager.cpp -o sappis_server -lpthread
./sappis_server

g++ -std=c++17 sappis_client.cpp ConfigManager.cpp -o sappis_client
./sappis_client


g++ -std=c++17 test_resources.cpp ResourceManager.cpp -o test_res
./test_res

g++ -std=c++17 test_preproc.cpp ConfigManager.cpp -o test_preproc -lstdc++fs
./test_preproc


# model, batchsize, pre_ms, inf_ms, threads, max_buff
vgg16, 1, 196800, 65600, 8, 1
alexnet, 1, 34968, 11656, 4, 3
alexnet, 2, 44358, 14786, 4, 2
alexnet, 4, 49896, 16632, 8, 1
simc2, 1, 4338, 1446, 4, 3
simc2, 2, 5547, 1849, 4, 3
simc2, 4, 6600, 2200, 8, 3
simc2, 8, 10599, 3533, 8, 3
hinet, 1, 3876, 1292, 2, 3
hinet, 2, 4653, 1551, 2, 3
hinet, 4, 5133, 1711, 4, 2
hinet, 8, 7134, 2378, 4, 2
hinet, 16, 9000, 3000, 8, 1


salloc \
  --job-name=SAPPIS_Edge_Node \
  --nodes=2 \
  --ntasks-per-node=1 \
  --cpus-per-task=24 \
  --mem=32G \
  --time=00:15:00 \
  --partition=genoa
# NO --exclusive flag


model, batch, pre_ms, inf_ms, threads, max_buff, file_mb, pre_mem_mb, inf_mem_mb
simc1, 1, 133, 3506, 1, 2, 20, 3, 25
simc1, 2, 242, 3882, 1, 2, 39, 4, 47
simc1, 4, 463, 4075, 1, 2, 77, 6, 92
simc1, 8, 907, 4319, 1, 2, 154, 9, 180
simc1, 16, 1804, 4642, 1, 2, 307, 17, 356

hinet, 1, 1356, 4866, 4, 2, 230, 9, 256
hinet, 1, 1366, 5895, 2, 2, 230, 9, 256
hinet, 1, 1363, 7506, 1, 2, 230, 9, 258

alexnet, 1, 15607, 24992, 4, 2, 6464, 6221, 8932
alexnet, 2, 24197, 28480, 4, 2, 7949, 6228, 10406
alexnet, 4, 41318, 25396, 8, 2, 10919, 6256, 13530
alexnet, 2, 24136, 21687, 8, 2, 7949, 6228, 10405

hinet, 1, 1369, 4752, 4, 2, 230, 9, 255
hinet, 2, 2682, 4986, 4, 2, 453, 13, 507
hinet, 4, 5361, 5458, 4, 2, 899, 33, 991
hinet, 8, 10636, 5919, 4, 2, 1791, 68, 1977
hinet, 16, 21283, 6277, 8, 2, 3575, 142, 3895


simc2, 1, 2227, 4858, 4, 2, 400, 39, 453
simc2, 2, 4597, 5221, 4, 2, 794, 79, 896
simc2, 4, 8826, 6359, 4, 2, 1581, 159, 1778
simc2, 4, 8854, 5376, 8, 2, 1581, 159, 1779
simc2, 8, 17648, 6731, 8, 2, 3157, 316, 3554


simc1, 1, 172, 3208, 4, 2, 20, 3, 25
simc1, 1, 130, 3399, 2, 2, 20, 3, 26
simc1, 1, 131, 3462, 1, 2, 20, 3, 25
simc1, 2, 246, 3556, 4, 2, 39, 4, 46
simc1, 2, 243, 3904, 2, 2, 39, 4, 48
simc1, 2, 243, 3810, 1, 2, 39, 4, 48
simc1, 4, 465, 3762, 4, 2, 77, 6, 89
simc1, 4, 462, 4134, 2, 2, 77, 6, 90
simc1, 8, 914, 3907, 4, 2, 154, 9, 180
simc1, 16, 1802, 4015, 4, 2, 307, 18, 353
simc1, 32, 3574, 4329, 4, 2, 614, 50, 701

1. Edge Server Specifications (Target Profiles)
Feature	Dell PowerEdge XR8000	HPE Edgeline EL8000	NVIDIA MGX (Edge Config)
Typical CPU	1x Intel Xeon Scalable (4th/5th Gen)	1x Intel Xeon (Ice Lake/Sapphire Rapids)	NVIDIA Grace (72-core) or Xeon 6
Cores	Up to 32 physical cores	16 to 32 cores per blade	64 to 72 cores
RAM	Up to 512 GB DDR5	Up to 768 GB DDR4/DDR5	128 GB to 480 GB (Unified)
Use Case	Ruggedized AI, 5G vRAN	Telco Edge, Video Analytics	Generative AI at the Edge
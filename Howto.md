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
  --cpus-per-task=32 \
  --mem=64G \
  --time=00:15:00 \
  --partition=genoa
# NO --exclusive flag




1. Edge Server Specifications (Target Profiles)
Feature	Dell PowerEdge XR8000	HPE Edgeline EL8000	NVIDIA MGX (Edge Config)
Typical CPU	1x Intel Xeon Scalable (4th/5th Gen)	1x Intel Xeon (Ice Lake/Sapphire Rapids)	NVIDIA Grace (72-core) or Xeon 6
Cores	Up to 32 physical cores	16 to 32 cores per blade	64 to 72 cores
RAM	Up to 512 GB DDR5	Up to 768 GB DDR4/DDR5	128 GB to 480 GB (Unified)
Use Case	Ruggedized AI, 5G vRAN	Telco Edge, Video Analytics	Generative AI at the Edge
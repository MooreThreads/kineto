# Usage:
# e.g:
# On machine A:
#   MASTER_ADDR=xxx MASTER_PORT=xxx python3 resnet50_distributed_ddp_profiler.py -n 2 -g 1 -nr 0
#
# On machine B:
#   MASTER_ADDR=xxx MASTER_PORT=xxx python3 resnet50_distributed_ddp_profiler.py -n 2 -g 1 -nr 1

import os
import argparse
import torch
from torch import nn
from torch import optim
from torch.nn.parallel import DistributedDataParallel as DDP
import torch.distributed as dist
import torch.multiprocessing as mp
import torch_musa
import torchvision
import torchvision.transforms as T
from torchvision import models

model = models.resnet50(weights=models.ResNet50_Weights.IMAGENET1K_V1)

transform = T.Compose([T.Resize(256), T.CenterCrop(224), T.ToTensor()])
trainset = torchvision.datasets.CIFAR10(root='./data', train=True,
                                        download=True, transform=transform)
trainloader = torch.utils.data.DataLoader(trainset, batch_size=32,
                                          shuffle=True, num_workers=4)
criterion = nn.CrossEntropyLoss()

def clean():
    dist.destroy_process_group()

def start(rank, world_size):
    if os.getenv("MASTER_ADDR") is None:
        os.environ['MASTER_ADDR'] = '127.0.0.1'
    if os.getenv("MASTER_PORT") is None:
        os.environ['MASTER_PORT'] = '29500'
    dist.init_process_group("mccl", rank=rank, world_size=world_size)

def runner(gpu, args):
    rank = args.nr * args.gpus + gpu
    torch_musa.set_device(rank % torch.musa.device_count())
    start(rank, args.world_size)
    model.to('musa')
    ddp_model = DDP(model, device_ids=[rank % torch.musa.device_count()])
    optimizer = optim.SGD(ddp_model.parameters(), lr=0.001)
    with torch.profiler.profile(
        activities=[
            torch.profiler.ProfilerActivity.CPU,
            torch.profiler.ProfilerActivity.MUSA],
        schedule=torch.profiler.schedule(
            wait=1,
            warmup=1,
            active=2),
        on_trace_ready=torch.profiler.tensorboard_trace_handler('./result', worker_name='worker'+str(rank)),
        record_shapes=True,
        profile_memory=True,  # This will take 1 to 2 minutes. Setting it to False could greatly speedup.
        with_stack=True,
        experimental_config=torch._C._profiler._ExperimentalConfig(verbose=True)
    ) as p:
        for step, data in enumerate(trainloader, 0):
            inputs, labels = data[0].to('musa'), data[1].to('musa')
            outputs = ddp_model(inputs)
            loss = criterion(outputs, labels)
            optimizer.zero_grad()
            loss.backward()
            optimizer.step()
            p.step()
            if step + 1 >= 4:
                break
    clean()

def train(fn, args):
    mp.spawn(fn, args=(args,), nprocs=args.gpus, join=True)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('-n', '--nodes', default=1,
                        type=int, metavar='N')
    parser.add_argument('-g', '--gpus', default=1, type=int,
                        help='number of gpus per node')
    parser.add_argument('-nr', '--nr', default=0, type=int,
                        help='ranking within the nodes')
    args = parser.parse_args()
    args.world_size = args.gpus * args.nodes
    train(runner, args)
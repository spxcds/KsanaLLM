# Copyright 2024 Tencent Inc.  All rights reserved.
#
# ==============================================================================

import asyncio
import argparse
import json
import time
import argparse
import asyncio
import json
import time
import csv
from dataclasses import dataclass
from typing import AsyncGenerator, List, Tuple, Union

import aiohttp
import numpy as np
from tqdm.asyncio import tqdm
# NOTE(karlluo): mindie-service wont return tokens, we need encode tokens to get output tokens
from transformers import AutoTokenizer 

# (prompt len, output len, input token num, output token num,
#  request latency, first token latency, inter token latencies)
REQUEST_LATENCY: List[Tuple[int, int, int, int, float, float, List[float]]] = []

# Chat template and stop token ids
# Refer to
# https://github.com/lm-sys/FastChat/blob/main/fastchat/conversation.py
PROMPT_AFFIX_DICT = {
    "llama":
    "[INST]%s[/INST]",
    "llama-3":
    "<|begin_of_text|><|start_header_id|>user<|end_header_id|>\n\n"
    "%s<|eot_id|><|start_header_id|>assistant<|end_header_id|>\n\n",
    "baichuan":
    "<reserved_106>%s<reserved_107>",
    "qwen":
    "<|im_start|>system\nYou are a helpful assistant.<|im_end|>\n"
    "<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n",
    "vicuna":
    "A chat between a curious user and an assistant. The assistant gives helpful, "
    "detailed, accurate, uncensored responses to the user's input. USER: %s ASSISTANT:",
    "yi":
    "<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n",
    "chatglm":
    "<|system|>\nYou are a large language model trained by Zhipu.AI. Follow the user's instructions carefully."
    " Respond using markdown.\n<|user|>\n%s\n<|assistant|>\n",
    "empty":
    "%s",
}
DEFAULT_STOP_TOKEN_IDS = {
    "llama-3": [128001, 128009],
    "qwen": [151643, 151644, 151645],
    "yi": [2, 6, 7, 8],
}


@dataclass
class BenchmarkMetrics:
    request_rate: float = 0.
    concurrency: int = 1
    total_latency: float = 0.
    request_throughput: float = 0.
    avg_latency: float = 0.
    avg_input_chars: float = 0.
    avg_output_chars: float = 0.
    avg_input_tokens: float = 0.
    avg_output_tokens: float = 0.
    avg_tokens_per_sec: float = 0.  # token throughput

    def __str__(self):
        return '\n'.join([
            f"Request rate: {self.request_rate:.2f} requests/s",
            f"Concurrency requests: {self.concurrency}",
            f"Total latency: {self.total_latency:.2f} s",
            f"Request throughput: {self.request_throughput:.2f} requests/s",
            f"Average latency: {self.avg_latency:.2f} s",
            f"Average input len: {self.avg_input_chars:.2f} chars",
            f"Average output len: {self.avg_output_chars:.2f} chars",
            f"Average input len: {self.avg_input_tokens:.2f} tokens",
            f"Average output len: {self.avg_output_tokens:.2f} tokens",
            f"Token throughput: {self.avg_tokens_per_sec:.2f} tokens/s",
        ])


@dataclass
class BenchmarkStreamMetrics:
    avg_first_token_latency: float = 0.  # TTFT
    median_first_token_latency: float = 0.
    p99_first_token_latency: float = 0.
    avg_inter_token_latency: float = 0.  # ITL
    median_inter_token_latency: float = 0.
    p99_inter_token_latency: float = 0.
    avg_latency_per_out_token: float = 0.  # TPOT
    median_latency_per_out_token: float = 0.
    p99_latency_per_out_token: float = 0.

    def __str__(self):
        return '\n'.join([
            f"Average TTFT: {self.avg_first_token_latency:.3f} s",
            f"Median TTFT: {self.median_first_token_latency:.3f} s",
            f"P99 TTFT: {self.p99_first_token_latency:.3f} s",
            f"Average ITL: {self.avg_inter_token_latency:.3f} s",
            f"Median ITL: {self.median_inter_token_latency:.3f} s",
            f"P99 ITL: {self.p99_inter_token_latency:.3f} s",
            f"Average TPOT: {self.avg_latency_per_out_token:.3f} s",
            f"Median TPOT: {self.median_latency_per_out_token:.3f} s",
            f"P99 TPOT: {self.p99_latency_per_out_token:.3f} s",
        ])


def args_config():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host',
                        type=str,
                        default="0.0.0.0",
                        help='server host address')
    parser.add_argument('--port', type=int, default=8888, help='server port')
    parser.add_argument('--input_csv',
                        type=str,
                        default="benchmark_input.csv",
                        help='input data for benchmark')
    parser.add_argument('--col_idx',
                        type=int,
                        default=0,
                        help='col_idx to be read from the input csv')
    parser.add_argument('--output_csv',
                        type=str,
                        default=None,
                        help='output csv file path')
    parser.add_argument('--perf_csv',
                        type=str,
                        default=None,
                        help='performance result csv file path')
    parser.add_argument("--request_rate",
                        type=float,
                        default=float("inf"),
                        help="Number of requests per second. If this is inf, "
                        "then all the requests are sent at time 0. "
                        "Otherwise, we synthesize the request arrival times.")
    parser.add_argument("--seed", type=int, default=0, help="random seed")
    parser.add_argument("--random",
                        action="store_true",
                        help="Randomize request arrival time.")
    parser.add_argument("--request_rate_step",
                        type=float,
                        default=1.0,
                        help="Step for changing the request rate in each iteration.")
    parser.add_argument("--request_rate_num_iters",
                        type=int,
                        default=1,
                        help="Number of iterations for changing the request rate.")
    parser.add_argument("--max_avg_latency",
                        type=float,
                        default=float("inf"),
                        help="The max average latency(seconds).")
    parser.add_argument("--max_first_token_latency",
                        type=float,
                        default=float("inf"),
                        help="The max average first token latency(seconds).")
    parser.add_argument("--concurrency",
                        type=int,
                        default=1,
                        help="Number of requests launched concurrently at the same time.")
    parser.add_argument("--warmup_num_iters",
                        type=int,
                        default=0,
                        help="Number of warmup iterations.")
    parser.add_argument("--repeat_num_iters",
                        type=int,
                        default=1,
                        help="Number of iterations to repeat.")
    parser.add_argument("--mode",
                        type=str,
                        default="async",
                        choices=['async', 'sync'],
                        help="requests send with async mode or sync mode")
    parser.add_argument('--stream',
                        action='store_true',
                        help='Whether to use stream mode for the request')
    parser.add_argument(
        '--backend',
        type=str,
        default="ksana",
        choices=[
            'ksana', 'vllm', 'ksana-server', 'vllm-server', 'trt-llm', 'evart', 'mindie-service'
        ],
        help='serving backend, ksana or vllm or evart or online server')
    parser.add_argument('--prompt_num',
                        type=int,
                        default=0,
                        help='number of input prompts')
    parser.add_argument(
        '--model_type',
        type=str,
        default="llama",
        choices=['llama', 'llama-3', 'baichuan', 'qwen', 'vicuna', 'yi', 'chatglm', 'empty'],
        help=
        'serving model type, used to add prefixes and suffixes to the prompt.')
    parser.add_argument('--max_new_tokens',
                        type=int,
                        default=1024,
                        help="The maximum numbers of tokens to generate, ignoring"
                             " the number of tokens in the prompt.")
    parser.add_argument('--temperature',
                        type=float,
                        default=0.0,
                        help="The value used to modulate the next token probabilities.")
    parser.add_argument('--topk',
                        type=int,
                        default=1,
                        help="The number of highest probability vocabulary tokens"
                             " to keep for top-k-filtering.")
    parser.add_argument('--topp',
                        type=float,
                        default=1.0,
                        help="If set to float < 1, only the smallest set of most"
                             " probable tokens with probabilities that add up to"
                             " top_p or higher are kept for generation.")
    parser.add_argument('--repetition_penalty',
                        type=float,
                        default=1.0,
                        help="The parameter for repetition penalty. 1.0 means no penalty.")
    parser.add_argument('--length_penalty',
                        type=float,
                        default=1.0,
                        help="Exponential penalty to the length that is used with"
                             " beam-based generation.")
    parser.add_argument('--num_beams',
                        type=int,
                        default=1,
                        help="Number of beams for beam search. 1 means no beam search.")
    parser.add_argument('--num_return_sequences',
                        type=int,
                        default=1,
                        help="The number of independently computed returned sequences"
                             " for each element in the batch.")
    parser.add_argument('--logprobs',
                        type=int,
                        default=0,
                        help="Whether to return log probabilities of the output tokens"
                             " or not. ")
    parser.add_argument('--stop_token_ids',
                        nargs='+',
                        type=int,
                        default=[],
                        help="A list of token id that should terminate generation if the"
                             " model outputs them.")
    parser.add_argument('--client_timeout',
                        type=int,
                        default=30*3600,
                        help="The timeout limit for the aiohttp client,"
                             "(default is 3 hour).")
    parser.add_argument('--tokenizer_path',
                        type=str,
                        default=None,
                        help="mindie-service/TensorRT-LLM wont return tokens, we need"
                             " encode tokens to get output tokens")
    args = parser.parse_args()
    return args


def read_from_csv(csv_file, col_idx=0, remove_head=True):
    import csv
    csv_reader = csv.reader(open(csv_file))
    if remove_head:
        next(csv_reader)
    return [row[col_idx] for row in csv_reader]


async def generate_prompt_async(
    input_requests: List[str],
    request_rate: float,
    concurrency: int,
    random: bool,
) -> AsyncGenerator[str, None]:
    input_requests = enumerate(input_requests)
    # Number of requests already sent at the same time
    request_num = 0
    for req_id, request in input_requests:
        yield req_id, request

        request_num += 1
        if request_rate == float("inf"):
            # If the request rate is infinity, then we don't need to wait.
            continue
        if request_num < concurrency:
            # If the number of sent requests is less than the required number of concurrencies,
            # then we don't need to wait either.
            continue
        request_num = 0

        if random:
            # Sample the request interval from the exponential distribution.
            interval = np.random.exponential(1.0 / (request_rate / concurrency))
        else:
            # Request arrives uniformly.
            interval = 1.0 / (request_rate / concurrency)
        # The next request will be sent after the interval.
        await asyncio.sleep(interval)


async def send_request_async(args: argparse.Namespace, prompt: str, api_url: str,
                             req_id: int, result_list: List, pbar: tqdm,
                             tokenizer: Union[None, AutoTokenizer]):
    headers = {"User-Agent": "Benchmark Client"}
    if not args.stop_token_ids:
        args.stop_token_ids = DEFAULT_STOP_TOKEN_IDS.get(args.model_type, [])
    if args.backend == "ksana":
        data = {
            "prompt": prompt,
            "sampling_config": {
                "temperature": args.temperature,
                "topk": args.topk,
                "topp": args.topp,
                "num_beams": args.num_beams,
                "num_return_sequences": args.num_return_sequences,
                "length_penalty": args.length_penalty,
                "repetition_penalty": args.repetition_penalty,
                "logprobs": args.logprobs,
                "max_new_tokens": args.max_new_tokens,
                "stop_token_ids": args.stop_token_ids
            },
            "stream": args.stream,
        }
    elif args.backend == "trt-llm":
        data = {
            "text_input": prompt,
            "max_tokens": args.max_new_tokens,
            "bad_words": "",
            "stop_words": "",
            "top_k": args.topk,
        }
    elif args.backend in ["vllm", "evart", "mindie-service"]:
        # max outputlen is 1024.
        data = {
            "prompt": prompt,
            "use_beam_search": False,
            "n": 1,
            "temperature": args.temperature,
            "max_tokens": args.max_new_tokens,
            "logprobs": args.logprobs,
            "repetition_penalty": args.repetition_penalty,
            "stop_token_ids": args.stop_token_ids,
            "skip_special_tokens": False,
            "spaces_between_special_tokens": False,
            "top_p": args.topp,
            "top_k": args.topk,
            "stream": args.stream
        }
    elif args.backend in ["ksana-server", "vllm-server"]:
        data = {
            "model": "default_model",
            "prompt": prompt,
            "top_p": args.topp,
            "temperature": args.temperature,
            "top_k": args.topk,
            "num_beams": args.num_beams,
            "repetition_penalty": args.repetition_penalty,
            "logprobs": args.logprobs,
            "n": 1,
            "task_id": time.time(),
            "delete_prompt_from_output": 0,
            "stream": args.stream,
            "stop_token_ids": args.stop_token_ids
        }

    # Set a timeout of 3 hours for the aiohttp client
    timeout = aiohttp.ClientTimeout(total=args.client_timeout)

    # Record the start time of the request
    request_start_time = time.perf_counter()

    # Create an asynchronous client session with the specified timeout
    async with aiohttp.ClientSession(timeout=timeout) as session:
        # Loop indefinitely until the request is successful
        while True:
            # Send a POST request to the API URL with the specified headers and data
            async with session.post(api_url, headers=headers,
                                    json=data) as response:
                # Store the last response chunk
                last_chunk = b""
                first_token_latency = 0.
                inter_token_latencies = []
                most_recent_timestamp = request_start_time
                # Iterate over the response chunks and append them to the list
                async for chunk, _ in response.content.iter_chunks():
                    chunk = chunk.strip(b'\x00')
                    if not chunk:
                        continue

                    timestamp = time.perf_counter()
                    # First token
                    if first_token_latency == 0.:
                        first_token_latency = timestamp - request_start_time
                    # Decoding phase
                    else:
                        inter_token_latencies.append(timestamp -
                                            most_recent_timestamp)
                    most_recent_timestamp = timestamp

                    last_chunk = chunk
            # Decode the last chunk to UTF-8
            output = last_chunk.decode("utf-8")
            # Parse the output as JSON
            if "server" in args.backend and args.stream:
                data_segments = output.strip().split("\n\n")
                texts = ""
                for segment in data_segments:
                    json_string = segment.split(': ', 1)[1]
                    data = json.loads(json_string)
                    texts += data["choices"][0]["delta"]["content"]
                output = json.loads(data_segments[-1].split(': ', 1)[1])
                output["choices"][0]["delta"]["content"] = texts

            output = json.loads(output)

            # If the response does not contain an "error" key, break out of the loop
            if "error" not in output:
                break

    # Record the end time of the request
    request_end_time = time.perf_counter()
    # Calculate the latency of the request
    request_latency = request_end_time - request_start_time

    output_token_num = len(output.get("output_token_ids", [""])[0])
    input_token_num = len(output.get("input_token_ids", ""))

    server_map_idx = "delta" if args.stream else "message"
    if args.backend == "ksana":
        output_text = output.get("texts", [""])[0].strip()
    elif args.backend == "trt-llm":
        prompt_len = len(prompt)
        output_text = output.get("text_output", "").strip()
        if tokenizer is None:
            input_token_num = 0
            output_token_num = 0
        else:
            input_token_num = len(tokenizer.encode(prompt))
            output_token_num = len(tokenizer.encode(output_text))
    elif args.backend == "vllm":
        prompt_len = len(prompt)
        output_text = output["text"][0][prompt_len:].strip()
        if tokenizer is None:
            input_token_num = 0
            output_token_num = 0
        else:
            input_token_num = len(tokenizer.encode(prompt))
            output_token_num = len(tokenizer.encode(output_text))
    elif args.backend == "evart":
        prompt_len = len(prompt)
        output_text = output["text"][0].strip()
        output_token_num = len(output.get("output_token_ids")[0])
    elif args.backend == "ksana-server":
        output_text = output['choices'][0][server_map_idx]['content']
        input_token_num = output['usage']['prompt_tokens']
        output_token_num = output['usage']['completion_tokens']
    elif args.backend == "vllm-server":
        prompt_len = len(prompt)
        output_text = output['choices'][0][server_map_idx]['content'][
            prompt_len:].strip()
    elif args.backend == "mindie-service":
        prompt_len = len(prompt)
        output_text = output["text"][0][prompt_len:].strip()
        if tokenizer is None:
            input_token_num = 0
            output_token_num = 0
        else:
            input_token_num = len(tokenizer.encode(prompt))
            output_token_num = len(tokenizer.encode(output_text))

    output_len = len(output_text)
    result_list[req_id] = output_text
    print("", output_text)
    REQUEST_LATENCY.append(
        (len(prompt), output_len if output_len > 0 else 1, input_token_num,
         output_token_num, request_latency,
         first_token_latency, inter_token_latencies))
    pbar.update(1)


# Define an asynchronous function to benchmark the API
async def benchmark_async(args: argparse.Namespace, api_url: str,
                          inputs: List[str], tokenizer: Union[None, AutoTokenizer]):
    # Initialize a list to store the asynchronous tasks
    tasks: List[asyncio.Task] = []
    # Create a progress bar with a total count equal to the number of inputs
    pbar = tqdm(total=len(inputs))
    # Initialize a result list with empty strings, one for each input
    result_list = [""] * len(inputs)
    # Asynchronously generate prompts with the specified request rate
    async for req_id, prompt in generate_prompt_async(inputs,
                                                      args.request_rate,
                                                      args.concurrency,
                                                      args.random):
        # Format the prompt using the affix dictionary for the specified model type
        prompt = PROMPT_AFFIX_DICT[args.model_type].replace("%s", prompt)
        # Create an asynchronous task to send the request
        task = asyncio.create_task(
            send_request_async(args, prompt, api_url, req_id, result_list, pbar,
                                tokenizer))
        # Add the task to the list of tasks
        tasks.append(task)
    # Wait for all tasks to complete
    await asyncio.gather(*tasks)
    # Close the progress bar
    pbar.close()
    # Return the result list
    return result_list


async def benchmark_sync(args: argparse.Namespace, api_url: str,
                         inputs: List[str], tokenizer: Union[None, AutoTokenizer]):
    # Create a progress bar with a total count equal to the number of inputs
    pbar = tqdm(total=len(inputs))
    # Initialize a result list with empty strings, one for each input
    result_list = [""] * len(inputs)
    # Asynchronously generate prompts with the specified request rate
    async for req_id, prompt in generate_prompt_async(inputs, args.request_rate,
                                                      args.concurrency,
                                                      args.random):
        # Format the prompt using the affix dictionary for the specified model type
        prompt = PROMPT_AFFIX_DICT[args.model_type].replace("%s", prompt)
        # Await until last request finished
        await send_request_async(args, prompt, api_url, req_id, result_list, pbar,
                                  tokenizer)
    # Close the progress bar
    pbar.close()
    # Return the result list
    return result_list


def adjust_list_length(inputs: List[str], args: argparse.Namespace):
    if args.prompt_num == 0:
        # 如果args.prompt_num为0，不做任何改变
        args.prompt_num = len(inputs)
        return inputs
    elif args.prompt_num > len(inputs):
        # 如果args.prompt_num大于列表长度，尝试复制列表
        repeat_times = args.prompt_num // len(inputs)
        if len(inputs) * repeat_times != args.prompt_num:
            # 如果无法通过整数倍复制达到指定长度，抛出错误
            print(f"len = {len(inputs)}, prompt_num = {args.prompt_num}")
            raise ValueError("无法通过整数倍复制达到指定长度")
        return inputs * repeat_times
    else:
        # 如果args.prompt_num小于或等于列表长度，截断列表
        return inputs[:args.prompt_num]


def run_benchmark(args: argparse.Namespace, api_url: str, inputs: List[str],
                  tokenizer: Union[None, AutoTokenizer]):
    if args.mode == "async":
        # Run the asynchronous benchmark
        return asyncio.run(benchmark_async(args, api_url, inputs, tokenizer))
    else:
        # Run the synchronous benchmark
        return asyncio.run(benchmark_sync(args, api_url, inputs, tokenizer))


def search_request_rate(args: argparse.Namespace, request_rate_list: List[Tuple[float, float]]):
    def round_to_tenth(number):
        # When searching for the optimal request rate, the minimum precision is 0.1.
        return max(round(number * 10) / 10, 0.1)
    step = len(request_rate_list)
    request_rate = -1
    if step < args.request_rate_num_iters:
        request_rate = args.request_rate + (args.request_rate_step if step > 0 else 0)
    elif args.max_avg_latency != float("inf") or args.max_first_token_latency != float("inf"):
        request_rate_list.sort(key=lambda x: x[0])
        if request_rate_list[-1][1] <= args.max_avg_latency and \
           request_rate_list[-1][2] <= args.max_first_token_latency:
            request_rate = min(request_rate_list[-1][0] * 2, args.prompt_num)
        elif request_rate_list[0][1] > args.max_avg_latency or \
             request_rate_list[0][2] > args.max_first_token_latency:
            request_rate = round_to_tenth(request_rate_list[0][0] / 2)
        else:
            rate_left = max(filter(lambda x: x[1] <= args.max_avg_latency and \
                                             x[2] <= args.max_first_token_latency, request_rate_list),
                                   key=lambda x: x[0])[0]
            rate_right = min(filter(lambda x: x[1] > args.max_avg_latency or \
                                              x[2] > args.max_first_token_latency, request_rate_list),
                                    key=lambda x: x[0])[0]
            request_rate = round_to_tenth((rate_left + rate_right) / 2)
        if any(ite[0] == request_rate for ite in request_rate_list):
            print(f"Duplicate request rate detected: {request_rate}. Terminating the search prematurely.")
            request_rate = -1
    return request_rate


def main(args: argparse.Namespace):
    global REQUEST_LATENCY

    np.random.seed(args.seed)

    tokenizer = None
    api_url = "http://" + args.host + ":" + str(args.port) + "/generate"
    if args.backend == "trt-llm":
        api_url = "http://" + args.host + ":" + str(
            args.port) + "/v2/models/ensemble/generate"
    elif args.backend in ["ksana-server", "vllm-server"]:
        api_url = "http://" + args.host + ":" + str(args.port) + "/v1/chat"
        args.model_type = "empty"  # 在线服务不需要手动拼接前后缀
    
    # NOTE: mindie-service/TensorRT-LLM wont return tokens, we need encode tokens to get output tokens
    if args.backend in ["mindie-service", "trt-llm", "vllm"]:
        tokenizer = AutoTokenizer.from_pretrained(
            args.tokenizer_path,
            revision=None,
            padding_side="left",
            truncation_side="left",
            trust_remote_code=True,
            use_fast=True
        )

    # Read inputs from the input CSV file
    inputs = read_from_csv(args.input_csv, args.col_idx)
    # Adjust the length of the input list based on the provided arguments
    inputs = adjust_list_length(inputs, args)

    perf_result_list: List[Tuple[BenchmarkMetrics, BenchmarkStreamMetrics]] = []
    # requst_rate_list: List[Tuple[request_rate, avg_latency, avg_TTFT]]
    request_rate_list: List[Tuple[float, float, float]] = []
    while True:
        metrics = BenchmarkMetrics()
        metrics.request_rate = search_request_rate(args, request_rate_list)
        args.request_rate = metrics.request_rate
        if metrics.request_rate == -1:
            break
        metrics.concurrency = args.concurrency
        for iter in range(args.warmup_num_iters):
            print(f"Start warmup iteration {iter} with request rate {metrics.request_rate:.3f}")
            run_benchmark(args, api_url, inputs, tokenizer)
        REQUEST_LATENCY.clear()

        # Record the start time of the benchmark
        benchmark_start_time = time.perf_counter()
        for iter in range(args.repeat_num_iters):
            print(f"Start profile iteration {iter} with request rate {metrics.request_rate:.3f}")
            result_list = run_benchmark(args, api_url, inputs, tokenizer)
        # Record the end time of the benchmark
        benchmark_end_time = time.perf_counter()

        # Calculate the total benchmark time
        metrics.total_latency = (
            benchmark_end_time - benchmark_start_time
        ) / args.repeat_num_iters
        # Calculate the request throughput
        metrics.request_throughput = len(inputs) / metrics.total_latency

        # Compute the latency statistics
        metrics.avg_latency = np.mean([latency for _, _, _, _, latency, _, _ in REQUEST_LATENCY])

        metrics.avg_input_chars = np.mean(
            [prompt_len for prompt_len, _, _, _, _, _, _ in REQUEST_LATENCY]
        )
        metrics.avg_output_chars = np.mean(
            [output_len for _, output_len, _, _, _, _, _ in REQUEST_LATENCY]
        )
        metrics.avg_input_tokens = np.mean(
            [input_tokens_num for _, _, input_tokens_num, _, _, _, _ in REQUEST_LATENCY]
        )
        metrics.avg_output_tokens = np.mean(
            [output_tokens_num for _, _, _, output_tokens_num, _, _, _ in REQUEST_LATENCY]
        )

        # Calculate the token throughput
        metrics.avg_tokens_per_sec = (metrics.avg_input_tokens + metrics.avg_output_tokens
            ) * len(inputs) / metrics.total_latency

        print(metrics)

        stream_metrics = BenchmarkStreamMetrics()
        if args.stream:  # TTFT, TPOT and ITL are only available in stream mode
            first_token_latencies = [
                first_token_latency
                for _, _, _, _, _, first_token_latency, _ in REQUEST_LATENCY
            ]
            stream_metrics.avg_first_token_latency = np.mean(first_token_latencies)
            stream_metrics.median_first_token_latency = np.median(first_token_latencies)
            stream_metrics.p99_first_token_latency = np.percentile(first_token_latencies, 99)

            inter_token_latencies = [
                inter_token_latency for _, _, _, _, _, _, inter_token_latencies in REQUEST_LATENCY
                for inter_token_latency in inter_token_latencies
            ]
            stream_metrics.avg_inter_token_latency = np.mean(inter_token_latencies)
            stream_metrics.median_inter_token_latency = np.median(inter_token_latencies)
            stream_metrics.p99_inter_token_latency = np.percentile(inter_token_latencies, 99)

            latencies_per_out_token = [
                (latency - first_token_latency) / (output_tokens_num - 1)
                for _, _, _, output_tokens_num, latency, first_token_latency, _ in REQUEST_LATENCY
                if output_tokens_num > 1
            ]
            if len(latencies_per_out_token) > 0:
                stream_metrics.avg_latency_per_out_token = np.mean(latencies_per_out_token)
                stream_metrics.median_latency_per_out_token = np.median(latencies_per_out_token)
                stream_metrics.p99_latency_per_out_token = np.percentile(latencies_per_out_token, 99)

            print(stream_metrics)

        perf_result_list.append((metrics, stream_metrics))
        request_rate_list.append((metrics.request_rate, metrics.avg_latency, stream_metrics.avg_first_token_latency))
        REQUEST_LATENCY.clear()

    if args.output_csv is not None:
        with open(args.output_csv, "w", newline='') as fs:
            writer = csv.writer(fs)
            for idx in range(len(result_list)):
                result = result_list[idx]
                writer.writerow([result.replace("</s>", "")])

    if args.perf_csv is not None:
        with open(args.perf_csv, "w", newline='') as fs:
            writer = csv.writer(fs)
            header = ["Request rate", "Concurrency", "Total latency", "Request throughput", "Avg latency",
                      "Avg input chars", "Avg output chars", "Avg input tokens", "Avg output tokens",
                      "Token throughput"]
            if args.stream:
                header.extend(["Avg TTFT", "Median TTFT", "P99 TTFT",
                               "Avg ITL", "Median ITL", "P99 ITL",
                               "Avg TPOT", "Median TPOT", "P99 TPOT"])
            writer.writerow(header)
            for (metrics, stream_metrics) in perf_result_list:
                row = [f"{value:.2f}" for value in metrics.__dict__.values()]
                if args.stream:
                    row.extend([f"{value:.3f}" for value in stream_metrics.__dict__.values()])
                writer.writerow(row)


if __name__ == "__main__":
    args = args_config()
    main(args)

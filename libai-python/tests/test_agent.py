import threading
import time
import unittest
from libai import Agent, ModelResponse, ToolCall

class BridgeTest(unittest.TestCase):
    def test_tools_unicode_history(self):
        requests, retained, chunks = [], [], []
        def model(request, invocation):
            requests.append(request)
            retained.append(invocation)
            if len(requests) == 1:
                return ModelResponse(calls=[ToolCall("编号", "echo", {"text": "你好🌏"})])
            invocation.emit("你好🌏")
            return ModelResponse(text="你好🌏", input_tokens=2, output_tokens=1)
        with Agent(model) as agent:
            agent.add_tool("echo", lambda call, inv: call.arguments["text"], requires_approval=True)
            result = agent.run("开始", authorize=lambda *args: True, on_text=lambda text: chunks.append(text) or True)
            self.assertEqual(result["status"], "completed")
            self.assertEqual(result["tool_calls"], 1)
            self.assertEqual(result["text"], "你好🌏")
            self.assertEqual(chunks, ["你好🌏"])
            agent.run("继续")
            self.assertGreater(len(requests[-1]["messages"]), 1)
            agent.reset()
            agent.run("新会话")
            self.assertEqual(len(requests[-1]["messages"]), 1)
            with self.assertRaises(RuntimeError): retained[0].stopped()
    def test_denial_budget_exception(self):
        count = []
        turns = [0]
        def model(request, inv):
            turns[0] += 1
            if turns[0] == 1: return ModelResponse(calls=[ToolCall("1", "write", {})])
            self.assertTrue(request["messages"][-1]["tool_error"])
            return ModelResponse(text="denied")
        with Agent(model) as agent:
            agent.add_tool("write", lambda *args: count.append(1) or "done", requires_approval=True)
            self.assertEqual(agent.run("start")["text"], "denied")
            self.assertEqual(count, [])
            self.assertEqual(agent.run("budget", max_model_calls=0)["status"], "budget_exceeded")
        def broken(*args): raise RuntimeError()
        with Agent(broken) as agent:
            self.assertEqual(agent.run("fail")["status"], "model_error")
    def test_concurrent_cancel(self):
        started = threading.Event()
        def model(request, inv):
            started.set()
            while not inv.stopped(): time.sleep(0.001)
            return ModelResponse()
        with Agent(model) as agent:
            results = []
            thread = threading.Thread(target=lambda: results.append(agent.run("wait")))
            thread.start()
            self.assertTrue(started.wait(5))
            with self.assertRaises(RuntimeError): agent.close()
            agent.cancel()
            thread.join(5)
            self.assertFalse(thread.is_alive())
            self.assertEqual(results[0]["status"], "cancelled")

if __name__ == "__main__": unittest.main()

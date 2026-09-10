"""Runtime 请求级单调绝对期限与兼容调用适配。"""

from __future__ import annotations

import inspect
import time
from dataclasses import dataclass, field
from typing import Callable


class RequestDeadlineExceeded(TimeoutError):
    """请求的统一绝对期限已经耗尽；消息不得包含后端细节。"""


@dataclass(frozen=True)
class MonotonicDeadline:
    """不可续期的单调绝对期限。

    ``clock`` 可在测试中替换；所有派生超时只会缩短，不会重新获得预算。
    """

    expires_at_ns: int
    clock_ns: Callable[[], int] = field(
        default=time.monotonic_ns, compare=False, repr=False)

    @classmethod
    def after_seconds(
            cls, seconds: float, *,
            clock_ns: Callable[[], int] = time.monotonic_ns,
    ) -> "MonotonicDeadline":
        if isinstance(seconds, bool) or not isinstance(seconds, (int, float)) \
                or seconds != seconds or seconds in (float("inf"),
                                                      float("-inf")) \
                or seconds <= 0:
            raise ValueError("请求期限必须是有限正数秒")
        duration_ns = int(float(seconds) * 1_000_000_000)
        if duration_ns < 1:
            raise ValueError("请求期限必须至少为1纳秒")
        now_ns = clock_ns()
        if isinstance(now_ns, bool) or not isinstance(now_ns, int) or \
                now_ns < 0:
            raise ValueError("单调时钟必须返回非负整数纳秒")
        return cls(now_ns + duration_ns, clock_ns)

    def remaining_seconds(self, *, cap: float | None = None) -> float:
        now_ns = self.clock_ns()
        if isinstance(now_ns, bool) or not isinstance(now_ns, int) or \
                now_ns < 0:
            raise RequestDeadlineExceeded("控制请求处理期限已耗尽")
        remaining_ns = self.expires_at_ns - now_ns
        if remaining_ns <= 0:
            raise RequestDeadlineExceeded("控制请求处理期限已耗尽")
        remaining = remaining_ns / 1_000_000_000
        if cap is not None:
            if isinstance(cap, bool) or not isinstance(cap, (int, float)) \
                    or cap != cap or cap in (float("inf"), float("-inf")) \
                    or cap <= 0:
                raise ValueError("阶段超时上限必须是有限正数秒")
            remaining = min(remaining, float(cap))
        return remaining

    def remaining_milliseconds(self, *, cap: int | None = None) -> int:
        # 向下取整，绝不把不足1ms的预算扩成1ms。
        now_ns = self.clock_ns()
        if isinstance(now_ns, bool) or not isinstance(now_ns, int):
            raise RequestDeadlineExceeded("控制请求处理期限已耗尽")
        remaining = (self.expires_at_ns - now_ns) // 1_000_000
        if remaining < 1:
            raise RequestDeadlineExceeded("控制请求处理期限已耗尽")
        return min(remaining, cap) if cap is not None else remaining

    def check(self) -> None:
        self.remaining_seconds()


def accepts_deadline(callback: Callable[..., object]) -> bool:
    """判断旧接口是否显式支持 ``deadline``，不靠捕获 TypeError 猜测。"""
    try:
        parameters = inspect.signature(callback).parameters.values()
    except (TypeError, ValueError):
        return False
    return any(
        parameter.kind == inspect.Parameter.VAR_KEYWORD or
        (parameter.name == "deadline" and parameter.kind in {
            inspect.Parameter.POSITIONAL_OR_KEYWORD,
            inspect.Parameter.KEYWORD_ONLY,
        })
        for parameter in parameters)


def call_with_deadline(callback: Callable[..., object], *arguments,
                       deadline: MonotonicDeadline | None = None,
                       **keywords):
    """向后兼容调用；新接口接收同一期限，旧接口在调用前后均受检查。"""
    if deadline is not None:
        deadline.check()
    try:
        if deadline is not None and accepts_deadline(callback):
            result = callback(*arguments, deadline=deadline, **keywords)
        else:
            result = callback(*arguments, **keywords)
    except Exception:
        # callback 已给出的精确错误比事后期限更强；不得用普通超时覆盖其中的
        # 提交状态。调用边界负责把“已开始但无精确结果”的超期转换为不确定错误。
        raise
    if deadline is not None:
        # 旧实现即使忽略子进程 timeout，也不能在期限后返回成功。
        deadline.check()
    return result

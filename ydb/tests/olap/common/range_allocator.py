from multiprocessing import Lock, Value

class RangeAllocator:

    class Range:
        def __init__(self, left, right):  # [left, right)
            self.left = left
            self.right = right

    def __init__(self, value=0):
        self._value = Value('i', int(value))
        self._lock = Lock()

    def allocate_range(self, length):
        with self._lock:
            left = self._value.value
            self._value.value += int(length)
            return RangeAllocator.Range(left, self._value.value)

    @property
    def get_border(self):
        with self._lock:
            return self._value

import time


class Item:
    __slots__ = ("base_price", "stock", "sold_today", "revenue")

    def __init__(self, base_price=0, stock=0, sold_today=0, revenue=0):
        self.base_price = base_price
        self.stock = stock
        self.sold_today = sold_today
        self.revenue = revenue

    def sell_one(self):
        if self.stock > 0:
            self.stock -= 1
            self.sold_today += 1
            self.revenue += self.base_price
            return self.base_price
        return 0

    def restock(self, qty):
        self.stock += qty
        self.sold_today = 0


class Customer:
    __slots__ = ("money", "items_bought", "spent")

    def __init__(self, money=0, items_bought=0, spent=0):
        self.money = money
        self.items_bought = items_bought
        self.spent = spent

    def purchase(self, item, qty):
        i = 0
        while i < qty:
            price = item.sell_one()
            if price > 0:
                if self.money >= price:
                    self.money -= price
                    self.spent += price
                    self.items_bought += 1
            i += 1


def run_market(customers=50_000):
    apple = Item(10, 100_000, 0, 0)
    bread = Item(25, 50_000, 0, 0)
    milk = Item(40, 30_000, 0, 0)
    total_revenue = 0
    total_items = 0
    restocks = 0

    i = 0
    while i < customers:
        cust = Customer(300, 0, 0)
        cust.purchase(apple, 3)
        cust.purchase(bread, 2)
        cust.purchase(milk, 1)
        total_revenue += cust.spent
        total_items += cust.items_bought

        if i % 500 == 0:
            apple.restock(50)
            bread.restock(30)
            milk.restock(20)
            restocks += 1

        i += 1

    return total_revenue, total_items, restocks, apple.stock, bread.stock, milk.stock


runs = 3
total_ms = 0.0
result = None

for _ in range(runs):
    start = time.perf_counter()
    result = run_market()
    total_ms += (time.perf_counter() - start) * 1000

avg = total_ms / runs
print("market sim 50000 customers")
print(f"result: {result}")
print(f"avg ({runs} runs): {avg:.2f} ms")

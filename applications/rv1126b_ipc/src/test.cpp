#include <concepts>
#include <iostream>
#include <vector>
#include <array>
#include <string>
#include <algorithm>
#include <ranges>
#include <functional>   // std::less

template<typename T>
concept Sortable = requires(T a, T b) {
    { a < b } -> std::convertible_to<bool>;
};

template<typename C>
concept SortableContainer = requires(C c) {
    requires Sortable<std::ranges::range_value_t<C>>;
    std::begin(c);
    std::end(c);
};

// 只用 operator< 语义，避免 ranges::less 对 == 的要求
void sort(SortableContainer auto& container) {
    std::ranges::sort(container, std::less<>{});
}

void sort(SortableContainer auto& container, auto comp) {
    std::ranges::sort(container, comp);
}

struct Point {
    int x, y;
    bool operator<(const Point& other) const {
        return std::tie(x, y) < std::tie(other.x, other.y);
    }
    // 若使用 ranges::less，还需要：
    bool operator==(const Point& other) const {
        return std::tie(x, y) == std::tie(other.x, other.y);
    }
};

struct NotComparable { int v; };

template<typename T>
void print(const T& c, const std::string& label) {
    std::cout << label << ": ";
    for (const auto& e : c) {
        if constexpr (requires { std::cout << e; })
            std::cout << e << ' ';
        else
            std::cout << "(" << e.x << "," << e.y << ") ";
    }
    std::cout << '\n';
}

int main() {
    std::vector<int> v{5, 2, 8, 1, 9, 3};
    sort(v);
    print(v, "vector<int>");

    std::vector<std::string> s{"banana", "apple", "cherry"};
    sort(s);
    print(s, "vector<string>");

    std::vector<Point> pts{{3, 1}, {1, 2}, {1, 1}, {2, 5}};
    sort(pts);
    print(pts, "vector<Point>");

    std::array<int, 5> arr{4, 1, 3, 5, 2};
    sort(arr);
    print(arr, "array<int>");

    std::vector<int> v2{1, 2, 3, 4, 5};
    sort(v2, std::greater<>{});
    print(v2, "vector<int> (desc)");

    static_assert(Sortable<int>);
    static_assert(Sortable<std::string>);
    static_assert(Sortable<Point>);
    static_assert(!Sortable<NotComparable>);

    std::cout << "All concepts check passed!\n";
    return 0;
}
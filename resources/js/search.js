
$(document).ready(function() {
	$(".search").val("");
	$(".search").on('submit', function() {
		var query = $(this).children(".search-input").val();
		window.location = "/search/" + $(this).data("volid") + "/" + encodeURIComponent(query);
		//$(this).children(".search-input").val("");
		return false;
	});
});

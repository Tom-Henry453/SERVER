console.log("Dashboard loaded");

document.querySelectorAll('.btn').forEach(button => {
    button.addEventListener('click', (e) => {
        // Optional: Intercept click to show animation or log
        console.log(`Requesting ${e.target.getAttribute('href')}...`);
    });
});

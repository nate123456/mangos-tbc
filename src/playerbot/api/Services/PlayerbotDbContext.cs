using Microsoft.EntityFrameworkCore;
using Playerbot.Api.Models;

namespace Playerbot.Api.Services;

public class PlayerbotDbContext : DbContext
{
    private readonly IConfiguration _configuration;

    public PlayerbotDbContext(IConfiguration configuration)
    {
        _configuration = configuration;
    }

    public DbSet<PlayerbotScript> Scripts { get; set; }
    public DbSet<PlayerbotToken> Tokens { get; set; }

    protected override void OnConfiguring(DbContextOptionsBuilder optionsBuilder)
    {
        var mysqlHost = _configuration["MYSQL_HOST"];
        var mysqlUser = _configuration["MYSQL_USER"];
        var mysqlPass = _configuration["MYSQL_PASS"];

        var connectionString = $@"server={mysqlHost};userid={mysqlUser};password={mysqlPass};database=tbccharacters";

        var serverVersion = new MySqlServerVersion(new Version(5, 7, 37));

        optionsBuilder.LogTo(Console.WriteLine);
        optionsBuilder.EnableSensitiveDataLogging();

        //optionsBuilder.UseMySql(connectionString, serverVersion)
        //    // The following three options help with debugging, but should
        //    // be changed or removed for production.
        //    .LogTo(Console.WriteLine, LogLevel.Information)
        //    .EnableSensitiveDataLogging()
        //    .EnableDetailedErrors();

        optionsBuilder.UseMySQL(connectionString);
    }

    protected override void OnModelCreating(ModelBuilder modelBuilder)
    {
        modelBuilder.Entity<PlayerbotScript>().HasKey(s => new { s.Name, s.AccountId });
    }
}
